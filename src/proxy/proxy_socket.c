/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_socket.h"
#include "server/render_protocol.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>

#define close closesocket
#define PROXY_SOCKET_MAX_FD_COUNT 8

/*
 * Windows implementation of proxy socket using TCP localhost.
 *
 * On Windows, Unix domain sockets and SCM_RIGHTS are not available.
 * Since we only use thread worker mode (in-process), file descriptors
 * are shared across threads and can be passed as inline data.
 *
 * Protocol for messages with fds:
 *   [uint32_t data_len][data bytes][uint32_t fd_count][int fds...]
 */

bool
proxy_socket_pair(int out_fds[static 2])
{
   WSADATA wsa;
   WSAStartup(MAKEWORD(2, 2), &wsa);

   SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (listener == INVALID_SOCKET)
      return false;

   struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
      .sin_port = 0,
   };

   if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
       listen(listener, 1)) {
      closesocket(listener);
      return false;
   }

   int addrlen = sizeof(addr);
   getsockname(listener, (struct sockaddr *)&addr, &addrlen);

   SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (client == INVALID_SOCKET) {
      closesocket(listener);
      return false;
   }

   if (connect(client, (struct sockaddr *)&addr, sizeof(addr))) {
      closesocket(client);
      closesocket(listener);
      return false;
   }

   SOCKET server = accept(listener, NULL, NULL);
   closesocket(listener);

   if (server == INVALID_SOCKET) {
      closesocket(client);
      return false;
   }

   out_fds[0] = (int)client;
   out_fds[1] = (int)server;
   return true;
}

bool
proxy_socket_is_valid(int fd)
{
   /* On Windows with TCP sockets, just check it's a valid socket */
   int type;
   int len = sizeof(type);
   return getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&type, &len) == 0;
}

void
proxy_socket_init(struct proxy_socket *socket, int fd)
{
   assert(fd >= 0);
   *socket = (struct proxy_socket){
      .fd = fd,
   };
}

void
proxy_socket_fini(struct proxy_socket *socket)
{
   closesocket((SOCKET)socket->fd);
}

bool
proxy_socket_is_connected(const struct proxy_socket *socket)
{
   /* Quick poll to check if socket is still open */
   fd_set readfds;
   struct timeval tv = { 0, 0 };
   FD_ZERO(&readfds);
   FD_SET((SOCKET)socket->fd, &readfds);
   int ret = select(0, &readfds, NULL, NULL, &tv);
   return ret >= 0;
}

static bool
send_all(SOCKET s, const void *data, size_t len)
{
   const char *p = data;
   while (len > 0) {
      int sent = send(s, p, (int)len, 0);
      if (sent <= 0)
         return false;
      p += sent;
      len -= sent;
   }
   return true;
}

static bool
recv_all(SOCKET s, void *data, size_t len)
{
   char *p = data;
   while (len > 0) {
      int got = recv(s, p, (int)len, 0);
      if (got <= 0)
         return false;
      p += got;
      len -= got;
   }
   return true;
}

bool
proxy_socket_receive_reply(struct proxy_socket *socket, void *data, size_t size)
{
   return recv_all((SOCKET)socket->fd, data, size);
}

bool
proxy_socket_receive_reply_with_fds(struct proxy_socket *socket,
                                    void *data,
                                    size_t size,
                                    int *fds,
                                    int max_fd_count,
                                    int *out_fd_count)
{
   if (!recv_all((SOCKET)socket->fd, data, size))
      return false;

   /* Receive inline fd count and fds (thread-local, no SCM_RIGHTS needed) */
   uint32_t fd_count = 0;
   if (!recv_all((SOCKET)socket->fd, &fd_count, sizeof(fd_count)))
      return false;

   if (fd_count > (uint32_t)max_fd_count) {
      proxy_log("too many fds: %u > %d", fd_count, max_fd_count);
      return false;
   }

   if (fd_count > 0) {
      if (!recv_all((SOCKET)socket->fd, fds, sizeof(int) * fd_count))
         return false;
   }

   if (out_fd_count)
      *out_fd_count = (int)fd_count;

   return true;
}

bool
proxy_socket_send_request(struct proxy_socket *socket, const void *data, size_t size)
{
   return send_all((SOCKET)socket->fd, data, size);
}

bool
proxy_socket_send_request_with_fds(struct proxy_socket *socket,
                                   const void *data,
                                   size_t size,
                                   const int *fds,
                                   int fd_count)
{
   if (!send_all((SOCKET)socket->fd, data, size))
      return false;

   /* Send fds inline (threads share fd table, no SCM_RIGHTS needed) */
   uint32_t count = (uint32_t)fd_count;
   if (!send_all((SOCKET)socket->fd, &count, sizeof(count)))
      return false;

   if (fd_count > 0) {
      if (!send_all((SOCKET)socket->fd, fds, sizeof(int) * fd_count))
         return false;
   }

   return true;
}

#else /* !_WIN32 */

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define PROXY_SOCKET_MAX_FD_COUNT 8

#ifdef __APPLE__
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif /* MSG_CMSG_CLOEXEC */
#endif /* __APPLE__ */

/* this is only used when the render server is started on demand */
bool
proxy_socket_pair(int out_fds[static 2])
{
   int type = SOCK_SEQPACKET;
#ifdef __APPLE__
   type = SOCK_STREAM;
#endif
   int ret = socketpair(AF_UNIX, type, 0, out_fds);
   if (ret) {
      proxy_log("failed to create socket pair");
      return false;
   }

   return true;
}

bool
proxy_socket_is_valid(int fd)
{
   int type;
   socklen_t len = sizeof(type);
   if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len)) {
      proxy_log("fd %d err %s", fd, strerror(errno));
      return false;
   }
#ifdef __APPLE__
   return type == SOCK_STREAM;
#else
   return type == SOCK_SEQPACKET;
#endif
}

void
proxy_socket_init(struct proxy_socket *socket, int fd)
{
   /* TODO make fd non-blocking and perform io with timeout */
   assert(fd >= 0);
   *socket = (struct proxy_socket){
      .fd = fd,
   };
}

void
proxy_socket_fini(struct proxy_socket *socket)
{
   close(socket->fd);
}

bool
proxy_socket_is_connected(const struct proxy_socket *socket)
{
   struct pollfd poll_fd = {
      .fd = socket->fd,
   };

   while (true) {
      const int ret = poll(&poll_fd, 1, 0);
      if (ret == 0) {
         return true;
      } else if (ret < 0) {
         if (errno == EINTR || errno == EAGAIN)
            continue;

         proxy_log("failed to poll socket");
         return false;
      }

      if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
         proxy_log("socket disconnected");
         return false;
      }

      return true;
   }
}

static const int *
get_received_fds(const struct msghdr *msg, int *out_count)
{
   const struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
   if (unlikely(!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0))) {
      *out_count = 0;
      return NULL;
   }

   *out_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
   return (const int *)CMSG_DATA(cmsg);
}

#ifdef __APPLE__
enum socket_state {
   SOCKET_STATE_FIRST_MSG,
   SOCKET_STATE_HEADER,
   SOCKET_STATE_DATA,
};
#endif

static bool
proxy_socket_recvmsg(struct proxy_socket *socket, struct msghdr *msg)
{
#ifdef __APPLE__
   enum socket_state state = SOCKET_STATE_FIRST_MSG;
   struct render_context_socket_header hdr = {0};
   ssize_t want = sizeof(hdr);
   struct msghdr _msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &hdr,
            .iov_len = want,
         },
      .msg_iovlen = 1,
      .msg_control = msg->msg_control,
      .msg_controllen = msg->msg_controllen,
   };
   socklen_t _msg_controllen;

   assert(msg->msg_iovlen == 1);

   do {
      const ssize_t s = recvmsg(socket->fd, &_msg, MSG_CMSG_CLOEXEC);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to receive message: %s", strerror(errno));
         return false;
      }
      if (unlikely(s == 0)) {
         proxy_log("socket disconnected");
         return false;
      }

      if (state == SOCKET_STATE_FIRST_MSG) {
         _msg_controllen = _msg.msg_controllen;
         state = SOCKET_STATE_HEADER;
      } else {
         /* retain the cmsg from first message */
         assert(_msg.msg_controllen == 0);
      }

      if (unlikely(_msg.msg_flags & MSG_CTRUNC)) {
         proxy_log("failed to receive message: truncated");

         int fd_count;
         const int *fds = get_received_fds(&_msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }

      if (s <= want) {
         _msg.msg_iov[0].iov_base = (char *)_msg.msg_iov[0].iov_base + s;
         _msg.msg_iov[0].iov_len -= s;
         want -= s;
      }

      if (!want && state == SOCKET_STATE_HEADER) {
         want = ntohl(hdr.length);
         _msg.msg_iov[0].iov_base = msg->msg_iov[0].iov_base;
         _msg.msg_iov[0].iov_len = want;
         state = SOCKET_STATE_DATA;
      } else if (!want && state == SOCKET_STATE_DATA) {
         msg->msg_controllen = _msg_controllen;
         break;
      }
   } while (true);
#else
   do {
      const ssize_t s = recvmsg(socket->fd, msg, MSG_CMSG_CLOEXEC);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to receive message: %s", strerror(errno));
         return false;
      }

      assert(msg->msg_iovlen == 1);
      if (unlikely((msg->msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
                   msg->msg_iov[0].iov_len != (size_t)s)) {
         proxy_log("failed to receive message: truncated or incomplete");

         int fd_count;
         const int *fds = get_received_fds(msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }

      return true;
   } while (true);
#endif

   return true;
}

static bool
proxy_socket_receive_reply_internal(struct proxy_socket *socket,
                                    void *data,
                                    size_t size,
                                    int *fds,
                                    int max_fd_count,
                                    int *out_fd_count)
{
   assert(data && size);
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * PROXY_SOCKET_MAX_FD_COUNT)];
   if (max_fd_count) {
      assert(fds && max_fd_count <= PROXY_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * max_fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      memset(cmsg, 0, sizeof(*cmsg));
   }

   if (!proxy_socket_recvmsg(socket, &msg))
      return false;

   if (max_fd_count) {
      int received_fd_count;
      const int *received_fds = get_received_fds(&msg, &received_fd_count);
      assert(received_fd_count <= max_fd_count);

      memcpy(fds, received_fds, sizeof(*fds) * received_fd_count);
      *out_fd_count = received_fd_count;
   } else if (out_fd_count) {
      *out_fd_count = 0;
   }

   return true;
}

bool
proxy_socket_receive_reply(struct proxy_socket *socket, void *data, size_t size)
{
   return proxy_socket_receive_reply_internal(socket, data, size, NULL, 0, NULL);
}

bool
proxy_socket_receive_reply_with_fds(struct proxy_socket *socket,
                                    void *data,
                                    size_t size,
                                    int *fds,
                                    int max_fd_count,
                                    int *out_fd_count)
{
   return proxy_socket_receive_reply_internal(socket, data, size, fds, max_fd_count,
                                              out_fd_count);
}

static bool
proxy_socket_sendmsg(struct proxy_socket *socket, const struct msghdr *msg)
{
#ifdef __APPLE__
   enum socket_state state = SOCKET_STATE_FIRST_MSG;
   struct render_context_socket_header hdr = {
      .length = htonl(msg->msg_iov[0].iov_len),
   };
   ssize_t want = sizeof(hdr);
   struct msghdr _msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &hdr,
            .iov_len = want,
         },
      .msg_iovlen = 1,
      .msg_control = msg->msg_control,
      .msg_controllen = msg->msg_controllen,
   };

   assert(msg->msg_iovlen == 1);

   do {
      const ssize_t s = sendmsg(socket->fd, &_msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to send message: %s", strerror(errno));
         return false;
      }
      if (unlikely(s == 0)) {
         proxy_log("failed to send message: socket disconnected");
         return false;
      }

      if (state == SOCKET_STATE_FIRST_MSG) {
         _msg.msg_controllen = 0;
         _msg.msg_control = NULL;
         state = SOCKET_STATE_HEADER;
      }

      if (s <= want) {
         _msg.msg_iov[0].iov_base = (char *)_msg.msg_iov[0].iov_base + s;
         _msg.msg_iov[0].iov_len -= s;
         want -= s;
      }

      if (!want && state == SOCKET_STATE_HEADER) {
         want = ntohl(hdr.length);
         _msg.msg_iov[0].iov_base = msg->msg_iov[0].iov_base;
         _msg.msg_iov[0].iov_len = want;
         state = SOCKET_STATE_DATA;
      } else if (!want && state == SOCKET_STATE_DATA) {
         return true;
      }
   } while (true);
#else
   do {
      const ssize_t s = sendmsg(socket->fd, msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to send message: %s", strerror(errno));
         return false;
      }

      /* no partial send since the socket type is SOCK_SEQPACKET */
      assert(msg->msg_iovlen == 1 && msg->msg_iov[0].iov_len == (size_t)s);
      return true;
   } while (true);
#endif
}

static bool
proxy_socket_send_request_internal(struct proxy_socket *socket,
                                   const void *data,
                                   size_t size,
                                   const int *fds,
                                   int fd_count)
{
   assert(data && size);
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = (void *)data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * PROXY_SOCKET_MAX_FD_COUNT)];
   if (fd_count) {
      assert(fds && fd_count <= PROXY_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(*fds) * fd_count);
      memcpy(CMSG_DATA(cmsg), fds, sizeof(*fds) * fd_count);
   }

   return proxy_socket_sendmsg(socket, &msg);
}

bool
proxy_socket_send_request(struct proxy_socket *socket, const void *data, size_t size)
{
   return proxy_socket_send_request_internal(socket, data, size, NULL, 0);
}

bool
proxy_socket_send_request_with_fds(struct proxy_socket *socket,
                                   const void *data,
                                   size_t size,
                                   const int *fds,
                                   int fd_count)
{
   return proxy_socket_send_request_internal(socket, data, size, fds, fd_count);
}

#endif /* _WIN32 */
