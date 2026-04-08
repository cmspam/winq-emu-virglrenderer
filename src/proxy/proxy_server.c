/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_server.h"

#ifdef _WIN32
#include <windows.h>
#include <errno.h>
#include <io.h>
#include "mman_win32.h"
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "server/render_protocol.h"

int
proxy_server_connect(struct proxy_server *srv)
{
   int client_fd = srv->client_fd;
   /* transfer ownership */
   srv->client_fd = -1;
   return client_fd;
}

void
proxy_server_destroy(struct proxy_server *srv)
{
#ifdef _WIN32
   /* The server thread will exit when the socket is closed */
#else
   if (srv->pid >= 0) {
      kill(srv->pid, SIGKILL);

      siginfo_t siginfo = { 0 };
      waitid(P_PID, srv->pid, &siginfo, WEXITED);
   }
#endif

   if (srv->client_fd >= 0) {
#ifdef _WIN32
      closesocket((SOCKET)srv->client_fd);
#else
      close(srv->client_fd);
#endif
   }

   free(srv);
}

#ifdef _WIN32
/*
 * Windows in-process render server
 *
 * On Unix, the render server runs as a separate process spawned via fork+exec.
 * On Windows, we run it as an in-process thread. Each Venus context gets its
 * own worker thread that calls vkr_renderer_* functions directly.
 *
 * Communication uses the same protocol (render_protocol.h) over TCP localhost
 * sockets. Since all threads share the process handle table, file
 * descriptors/handles can be passed as inline integers.
 */

#include "c11/threads.h"
#include "vkr_renderer.h"
#include "virgl_util.h"

/* matches PROXY_CONTEXT_TIMELINE_COUNT in proxy_context.h */
#define SERVER_TIMELINE_COUNT 64

struct server_context_worker {
   uint32_t ctx_id;
   char name[32];
   uint32_t name_len;
   int socket_fd;

   /* shared memory for timeline seqnos (mapped by proxy_context) */
   void *shmem_ptr;
   size_t shmem_size;
   int shmem_fd;
   atomic_uint *timeline_seqnos;
   int timeline_count;
   int fence_eventfd;

   thrd_t thread;
   struct list_head head;
};

struct server_thread_state {
   int socket_fd;
   uint32_t init_flags;

   mtx_t renderer_mutex;
   bool vkr_initialized;

   mtx_t workers_mutex;
   struct list_head workers;
};

/* global state for the retire_fence callback */
static struct server_thread_state *g_server_state;

static bool
server_recv_all(int fd, void *data, size_t len)
{
   char *p = data;
   while (len > 0) {
      int got = recv((SOCKET)fd, p, (int)len, 0);
      if (got <= 0)
         return false;
      p += got;
      len -= got;
   }
   return true;
}

static bool
server_send_all(int fd, const void *data, size_t len)
{
   const char *p = data;
   while (len > 0) {
      int sent = send((SOCKET)fd, p, (int)len, 0);
      if (sent <= 0)
         return false;
      p += sent;
      len -= sent;
   }
   return true;
}

static bool
server_recv_with_fds(int fd, void *data, size_t size,
                     int *fds, int max_fds, int *out_fd_count)
{
   if (!server_recv_all(fd, data, size))
      return false;

   uint32_t fd_count = 0;
   if (!server_recv_all(fd, &fd_count, sizeof(fd_count)))
      return false;

   if ((int)fd_count > max_fds) {
      proxy_log("server: too many fds: %u", fd_count);
      return false;
   }

   if (fd_count > 0) {
      if (!server_recv_all(fd, fds, sizeof(int) * fd_count))
         return false;
   }

   *out_fd_count = (int)fd_count;
   return true;
}

static bool
server_send_reply(int fd, const void *data, size_t size)
{
   return server_send_all(fd, data, size);
}

static bool
server_send_reply_with_fds(int fd, const void *data, size_t size,
                           const int *fds, int fd_count)
{
   if (!server_send_all(fd, data, size))
      return false;

   uint32_t count = (uint32_t)fd_count;
   if (!server_send_all(fd, &count, sizeof(count)))
      return false;

   if (fd_count > 0) {
      if (!server_send_all(fd, fds, sizeof(int) * fd_count))
         return false;
   }
   return true;
}

static struct server_context_worker *
server_find_worker(struct server_thread_state *state, uint32_t ctx_id)
{
   list_for_each_entry(struct server_context_worker, w, &state->workers, head) {
      if (w->ctx_id == ctx_id)
         return w;
   }
   return NULL;
}

/*
 * VKR fence retirement callback.
 *
 * Called from VKR sync threads when a fence signals. Updates the shared
 * memory timeline seqno and optionally signals the eventfd.
 */
static void
server_retire_fence_cb(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
   struct server_thread_state *state = g_server_state;
   if (!state)
      return;

   mtx_lock(&state->workers_mutex);
   struct server_context_worker *w = server_find_worker(state, ctx_id);
   if (w && w->timeline_seqnos && (int)ring_idx < w->timeline_count) {
      uint32_t seqno = (uint32_t)fence_id;
      atomic_store(&w->timeline_seqnos[ring_idx], seqno);
      if (w->fence_eventfd >= 0)
         write_eventfd(w->fence_eventfd, 1);
   }
   mtx_unlock(&state->workers_mutex);
}

static void
server_cb_debug_logger(UNUSED enum virgl_log_level_flags log_level,
                       const char *message,
                       UNUSED void *user_data)
{
   fputs("vkr: ", stderr);
   fputs(message, stderr);
   fflush(stderr);

   FILE *f = fopen("venus-server.log",
                   "a");
   if (f) {
      fputs("vkr: ", f);
      fputs(message, f);
      fclose(f);
   }
}

static const struct vkr_renderer_callbacks server_vkr_cbs = {
   .debug_logger = server_cb_debug_logger,
   .retire_fence = server_retire_fence_cb,
};

static FILE *
server_dbgf(const char *fmt, ...)
{
   static FILE *f = NULL;
   if (!f)
      f = fopen("venus-server.log", "a");
   if (f) {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(f, fmt, ap);
      va_end(ap);
      fflush(f);
   }
   return f;
}

static bool
server_ensure_vkr_init(struct server_thread_state *state)
{
   mtx_lock(&state->renderer_mutex);
   if (!state->vkr_initialized) {
      static const uint32_t vkr_flags =
         VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB;
      server_dbgf("vkr_renderer_init flags=0x%x\n", vkr_flags);
      if (!vkr_renderer_init(vkr_flags, &server_vkr_cbs)) {
         mtx_unlock(&state->renderer_mutex);
         server_dbgf("vkr_renderer_init FAILED\n");
         proxy_log("server: failed to init vkr renderer");
         return false;
      }
      state->vkr_initialized = true;
      server_dbgf("vkr_renderer_init OK\n");
   }
   mtx_unlock(&state->renderer_mutex);
   return true;
}

/*
 * Context worker thread — handles per-context operations.
 *
 * Each Venus context gets its own worker thread that processes commands
 * from the proxy_context on the QEMU side.
 */
static int
server_context_worker_thread(void *arg)
{
   struct server_context_worker *w = arg;
   struct server_thread_state *state = g_server_state;
   bool ctx_created = false;

   while (true) {
      struct render_context_op_header hdr;
      if (!server_recv_all(w->socket_fd, &hdr, sizeof(hdr))) {
         break;
      }

      switch (hdr.op) {
      case RENDER_CONTEXT_OP_INIT: {
         /* Read remaining fields after header.
          * Must read fields individually to avoid struct padding issues
          * (uint32_t followed by size_t gets 4 bytes of padding on 64-bit).
          */
         uint32_t init_flags;
         size_t init_shmem_size;
         if (!server_recv_all(w->socket_fd, &init_flags, sizeof(init_flags)))
            goto done;
         if (!server_recv_all(w->socket_fd, &init_shmem_size, sizeof(init_shmem_size)))
            goto done;

         int fds[2];
         uint32_t fd_count = 0;
         if (!server_recv_all(w->socket_fd, &fd_count, sizeof(fd_count)))
            goto done;
         if (fd_count < 1 || fd_count > 2) {
            proxy_log("server worker: bad fd count %u for INIT", fd_count);
            goto done;
         }
         if (!server_recv_all(w->socket_fd, fds, sizeof(int) * fd_count))
            goto done;

         w->shmem_fd = fds[0];
         w->fence_eventfd = (fd_count >= 2) ? fds[1] : -1;
         w->shmem_size = init_shmem_size;
         w->timeline_count = (int)(init_shmem_size / sizeof(atomic_uint));

         /* Map shared memory through mmap so both CRT fds and any Windows
          * handle-backed fd variants follow the same path.
          */
         w->shmem_ptr = mmap(NULL, w->shmem_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, w->shmem_fd, 0);
         if (w->shmem_ptr == MAP_FAILED) {
            goto done;
         }
         w->timeline_seqnos = (atomic_uint *)w->shmem_ptr;

         for (int i = 0; i < w->timeline_count; i++)
            atomic_store(&w->timeline_seqnos[i], 0);

         /* Initialize VKR renderer if needed */
         if (!server_ensure_vkr_init(state)) {
            goto done;
         }

         /* Create the VKR context */
         mtx_lock(&state->renderer_mutex);
         bool ok = vkr_renderer_create_context(w->ctx_id, init_flags,
                                               w->name_len, w->name);
         mtx_unlock(&state->renderer_mutex);
         if (!ok) {
            proxy_log("server worker: failed to create vkr context %u", w->ctx_id);
            goto done;
         }
         ctx_created = true;
         break;
      }

      case RENDER_CONTEXT_OP_SUBMIT_CMD: {
         struct {
            uint32_t size;
            char cmd[256];
         } cmd_body;
         if (!server_recv_all(w->socket_fd, &cmd_body,
                              sizeof(cmd_body.size) + sizeof(cmd_body.cmd)))
            goto done;

         void *cmd = cmd_body.cmd;
         void *large_cmd = NULL;
         if (cmd_body.size > sizeof(cmd_body.cmd)) {
            large_cmd = malloc(cmd_body.size);
            if (!large_cmd)
               goto done;
            memcpy(large_cmd, cmd_body.cmd, sizeof(cmd_body.cmd));
            size_t remain = cmd_body.size - sizeof(cmd_body.cmd);
            if (!server_recv_all(w->socket_fd,
                                 (char *)large_cmd + sizeof(cmd_body.cmd),
                                 remain)) {
               free(large_cmd);
               goto done;
            }
            cmd = large_cmd;
         }

         mtx_lock(&state->renderer_mutex);
         bool cmd_ok = vkr_renderer_submit_cmd(w->ctx_id, cmd, cmd_body.size);
         mtx_unlock(&state->renderer_mutex);
         (void)cmd_ok;

         free(large_cmd);
         break;
      }

      case RENDER_CONTEXT_OP_SUBMIT_FENCE: {
         struct {
            uint32_t flags;
            uint32_t ring_index;
            uint32_t seqno;
         } fence_body;
         if (!server_recv_all(w->socket_fd, &fence_body, sizeof(fence_body)))
            goto done;

         mtx_lock(&state->renderer_mutex);
         vkr_renderer_submit_fence(w->ctx_id,
                                   VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                   fence_body.ring_index,
                                   fence_body.seqno);
         mtx_unlock(&state->renderer_mutex);
         break;
      }

      case RENDER_CONTEXT_OP_CREATE_RESOURCE: {
         /* Read fields individually to avoid struct padding between
          * uint32_t res_id and uint64_t blob_id (4 bytes padding on 64-bit).
          */
         uint32_t res_id;
         uint64_t blob_id;
         uint64_t blob_size;
         uint32_t blob_flags;
         if (!server_recv_all(w->socket_fd, &res_id, sizeof(res_id)) ||
             !server_recv_all(w->socket_fd, &blob_id, sizeof(blob_id)) ||
             !server_recv_all(w->socket_fd, &blob_size, sizeof(blob_size)) ||
             !server_recv_all(w->socket_fd, &blob_flags, sizeof(blob_flags)))
            goto done;

         struct render_context_op_create_resource_reply reply = {
            .fd_type = VIRGL_RESOURCE_FD_INVALID,
         };
         int res_fd = -1;

         mtx_lock(&state->renderer_mutex);
         bool ok = vkr_renderer_create_resource(
            w->ctx_id, res_id, blob_id,
            blob_size, blob_flags,
            &reply.fd_type, &res_fd, &reply.map_info,
            &reply.vulkan_info);
         mtx_unlock(&state->renderer_mutex);

         if (!ok || res_fd < 0) {
            reply.fd_type = VIRGL_RESOURCE_FD_INVALID;
            server_send_reply(w->socket_fd, &reply, sizeof(reply));
         } else {
            server_send_reply_with_fds(w->socket_fd, &reply, sizeof(reply),
                                       &res_fd, 1);
         }
         break;
      }

      case RENDER_CONTEXT_OP_IMPORT_RESOURCE: {
         struct {
            uint32_t res_id;
            enum virgl_resource_fd_type fd_type;
            uint64_t size;
         } imp_body;

         int fds[1];
         int fd_count;
         if (!server_recv_with_fds(w->socket_fd, &imp_body, sizeof(imp_body),
                                   fds, 1, &fd_count))
            goto done;

         if (fd_count != 1) {
            proxy_log("server worker: import resource missing fd");
            break;
         }

         mtx_lock(&state->renderer_mutex);
         vkr_renderer_import_resource(w->ctx_id, imp_body.res_id,
                                      imp_body.fd_type, fds[0], imp_body.size);
         mtx_unlock(&state->renderer_mutex);
         break;
      }

      case RENDER_CONTEXT_OP_DESTROY_RESOURCE: {
         struct {
            uint32_t res_id;
         } destroy_body;
         if (!server_recv_all(w->socket_fd, &destroy_body, sizeof(destroy_body)))
            goto done;

         mtx_lock(&state->renderer_mutex);
         vkr_renderer_destroy_resource(w->ctx_id, destroy_body.res_id);
         mtx_unlock(&state->renderer_mutex);
         break;
      }

      case RENDER_CONTEXT_OP_NOP:
         break;

      default:
         proxy_log("server worker: unknown op %d", hdr.op);
         goto done;
      }
   }

done:
   proxy_log("server worker: exiting ctx %u", w->ctx_id);

   if (ctx_created) {
      mtx_lock(&state->renderer_mutex);
      vkr_renderer_destroy_context(w->ctx_id);
      mtx_unlock(&state->renderer_mutex);
   }

   if (w->shmem_ptr)
      UnmapViewOfFile(w->shmem_ptr);

   closesocket((SOCKET)w->socket_fd);
   return 0;
}

static void
server_destroy_worker(struct server_thread_state *state,
                      struct server_context_worker *w)
{
   /* Close our end; worker thread will exit when recv fails */
   closesocket((SOCKET)w->socket_fd);
   thrd_join(w->thread, NULL);

   list_del(&w->head);
   free(w);
}

/*
 * Server main thread — handles client protocol messages.
 *
 * This is the Windows replacement for the forked render_server process.
 */
static int
server_main_thread(void *arg)
{
   struct server_thread_state *state = arg;

   {
      /* Debug: write directly to a file to confirm thread started */
      FILE *tf = fopen("venus-thread.log", "w");
      if (tf) {
         fprintf(tf, "server_main_thread STARTED fd=%d\n", state->socket_fd);
         fflush(tf);
         fclose(tf);
      }
   }

   while (true) {
      struct render_client_op_header hdr;
      if (!server_recv_all(state->socket_fd, &hdr, sizeof(hdr))) {
         break;
      }

      switch (hdr.op) {
      case RENDER_CLIENT_OP_INIT: {
         uint32_t flags;
         if (!server_recv_all(state->socket_fd, &flags, sizeof(flags)))
            goto done;
         state->init_flags = flags;
         break;
      }

      case RENDER_CLIENT_OP_CREATE_CONTEXT: {
         struct {
            uint32_t ctx_id;
            char ctx_name[32];
         } ctx_body;
         if (!server_recv_all(state->socket_fd, &ctx_body, sizeof(ctx_body)))
            goto done;

         /* Create socketpair for worker <-> proxy_context */
         int socket_fds[2];
         if (!proxy_socket_pair(socket_fds)) {
            struct render_client_op_create_context_reply reply = { .ok = false };
            server_send_reply_with_fds(state->socket_fd, &reply, sizeof(reply),
                                       NULL, 0);
            break;
         }

         struct server_context_worker *w = calloc(1, sizeof(*w));
         if (!w) {
            closesocket((SOCKET)socket_fds[0]);
            closesocket((SOCKET)socket_fds[1]);
            struct render_client_op_create_context_reply reply = { .ok = false };
            server_send_reply_with_fds(state->socket_fd, &reply, sizeof(reply),
                                       NULL, 0);
            break;
         }

         w->ctx_id = ctx_body.ctx_id;
         w->name_len = (uint32_t)strnlen(ctx_body.ctx_name,
                                         sizeof(ctx_body.ctx_name) - 1);
         memcpy(w->name, ctx_body.ctx_name, w->name_len);
         w->name[w->name_len] = '\0';
         w->socket_fd = socket_fds[0];
         w->shmem_fd = -1;
         w->fence_eventfd = -1;

         int ret = thrd_create(&w->thread, server_context_worker_thread, w);
         if (ret != thrd_success) {
            proxy_log("server thread: failed to create worker thread");
            closesocket((SOCKET)socket_fds[0]);
            closesocket((SOCKET)socket_fds[1]);
            free(w);
            struct render_client_op_create_context_reply reply = { .ok = false };
            server_send_reply_with_fds(state->socket_fd, &reply, sizeof(reply),
                                       NULL, 0);
            break;
         }

         mtx_lock(&state->workers_mutex);
         list_addtail(&w->head, &state->workers);
         mtx_unlock(&state->workers_mutex);

         /* Send back the other end of the socketpair */
         int remote_fd = socket_fds[1];
         struct render_client_op_create_context_reply reply = { .ok = true };
         server_send_reply_with_fds(state->socket_fd, &reply, sizeof(reply),
                                    &remote_fd, 1);
         break;
      }

      case RENDER_CLIENT_OP_DESTROY_CONTEXT: {
         uint32_t ctx_id;
         if (!server_recv_all(state->socket_fd, &ctx_id, sizeof(ctx_id)))
            goto done;

         mtx_lock(&state->workers_mutex);
         struct server_context_worker *w = server_find_worker(state, ctx_id);
         if (w)
            server_destroy_worker(state, w);
         mtx_unlock(&state->workers_mutex);
         break;
      }

      case RENDER_CLIENT_OP_RESET: {
         mtx_lock(&state->workers_mutex);
         list_for_each_entry_safe(struct server_context_worker, w,
                                  &state->workers, head)
            server_destroy_worker(state, w);
         mtx_unlock(&state->workers_mutex);
         break;
      }

      case RENDER_CLIENT_OP_NOP:
         break;

      default:
         proxy_log("server thread: unknown op %d", hdr.op);
         goto done;
      }
   }

done:
   proxy_log("server thread: exiting");

   /* Clean up remaining workers */
   mtx_lock(&state->workers_mutex);
   list_for_each_entry_safe(struct server_context_worker, w,
                            &state->workers, head)
      server_destroy_worker(state, w);
   mtx_unlock(&state->workers_mutex);

   if (state->vkr_initialized)
      vkr_renderer_fini();

   closesocket((SOCKET)state->socket_fd);
   mtx_destroy(&state->renderer_mutex);
   mtx_destroy(&state->workers_mutex);
   free(state);
   return 0;
}

static bool
proxy_server_create_thread(struct proxy_server *srv)
{
   server_dbgf("proxy_server_create_thread: entered\n");

   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds)) {
      server_dbgf("proxy_server_create_thread: socket_pair failed\n");
      return false;
   }
   server_dbgf("proxy_server_create_thread: socket_pair OK fds=%d,%d\n",
               socket_fds[0], socket_fds[1]);

   struct server_thread_state *state = calloc(1, sizeof(*state));
   if (!state) {
      closesocket((SOCKET)socket_fds[0]);
      closesocket((SOCKET)socket_fds[1]);
      return false;
   }

   state->socket_fd = socket_fds[1];
   mtx_init(&state->renderer_mutex, mtx_plain);
   mtx_init(&state->workers_mutex, mtx_plain);
   list_inithead(&state->workers);

   g_server_state = state;

   thrd_t thread;
   if (thrd_create(&thread, server_main_thread, state) != thrd_success) {
      proxy_log("failed to create server thread");
      closesocket((SOCKET)socket_fds[0]);
      closesocket((SOCKET)socket_fds[1]);
      mtx_destroy(&state->renderer_mutex);
      mtx_destroy(&state->workers_mutex);
      free(state);
      return false;
   }
   thrd_detach(thread);

   srv->client_fd = socket_fds[0];
   srv->thread_data = state;
   return true;
}

#else /* !_WIN32 */

static bool
proxy_server_fork(struct proxy_server *srv)
{
   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds))
      return false;
   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   pid_t pid = fork();
   if (pid < 0) {
      proxy_log("failed to fork proxy server");
      close(client_fd);
      close(remote_fd);
      return false;
   }

   if (pid > 0) {
      srv->pid = pid;
      srv->client_fd = client_fd;
      close(remote_fd);
   } else {
      close(client_fd);

      /* do not receive signals from terminal */
      setpgid(0, 0);

      char fd_str[16];
      snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);

      /* for devenv without installing server */
      char *const server_path = getenv("RENDER_SERVER_EXEC_PATH");
      char *const argv[] = {
         server_path ? server_path : RENDER_SERVER_EXEC_PATH,
         "--socket-fd",
         fd_str,
         NULL,
      };
      execv(argv[0], argv);

      proxy_log("failed to exec %s: %s", argv[0], strerror(errno));
      close(remote_fd);
      exit(-1);
   }

   return true;
}

#endif /* !_WIN32 */

static bool
proxy_server_init_fd(struct proxy_server *srv)
{
   /* the fd represents a connection to the server */
   srv->client_fd = proxy_renderer.cbs->get_server_fd(RENDER_SERVER_VERSION);
   if (srv->client_fd < 0)
      return false;

   return true;
}

struct proxy_server *
proxy_server_create(void)
{
   server_dbgf("proxy_server_create called\n");
   struct proxy_server *srv = calloc(1, sizeof(*srv));
   if (!srv)
      return NULL;

#ifndef _WIN32
   srv->pid = -1;
#endif

   if (!proxy_server_init_fd(srv)) {
#ifdef _WIN32
      /* Start an in-process render server thread */
      if (!proxy_server_create_thread(srv)) {
         free(srv);
         return NULL;
      }
#else
      /* start the render server on demand when the client does not provide a
       * server fd
       */
      if (!proxy_server_fork(srv)) {
         free(srv);
         return NULL;
      }
#endif
   }

   if (!proxy_socket_is_valid(srv->client_fd)) {
      proxy_log("invalid client fd type");
#ifdef _WIN32
      closesocket((SOCKET)srv->client_fd);
#else
      close(srv->client_fd);
#endif
      free(srv);
      return NULL;
   }

#ifndef _WIN32
   proxy_log("proxy server with pid %d", srv->pid);
#else
   proxy_log("proxy server running as in-process thread");
#endif

   return srv;
}
