/*
 * Copyright 2019 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "os_file.h"
#include "detect_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#if DETECT_OS_WINDOWS
#include <windows.h>
#include <io.h>
#define open _open
#define fdopen _fdopen
#define O_CREAT _O_CREAT
#define O_EXCL _O_EXCL
#define O_WRONLY _O_WRONLY
#else
#include <unistd.h>
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#endif

#if DETECT_OS_WINDOWS
struct os_win32_handle_entry {
   int token;
   HANDLE handle;
};

static CRITICAL_SECTION os_win32_handle_mutex;
static INIT_ONCE os_win32_handle_once = INIT_ONCE_STATIC_INIT;
static struct os_win32_handle_entry *os_win32_handles;
static size_t os_win32_handle_count;
static size_t os_win32_handle_capacity;
static LONG os_win32_next_token = 0x40000000;

static BOOL CALLBACK
os_win32_handle_init_once(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context)
{
   (void)InitOnce;
   (void)Parameter;
   (void)Context;
   InitializeCriticalSection(&os_win32_handle_mutex);
   return TRUE;
}

static void
os_win32_handle_ensure_init(void)
{
   InitOnceExecuteOnce(&os_win32_handle_once, os_win32_handle_init_once, NULL, NULL);
}

static ptrdiff_t
os_win32_handle_find_index_locked(int token)
{
   for (size_t i = 0; i < os_win32_handle_count; i++) {
      if (os_win32_handles[i].token == token)
         return (ptrdiff_t)i;
   }

   return -1;
}

static int
os_win32_handle_insert(HANDLE handle)
{
   os_win32_handle_ensure_init();
   EnterCriticalSection(&os_win32_handle_mutex);

   if (os_win32_handle_count == os_win32_handle_capacity) {
      size_t new_cap = os_win32_handle_capacity ? os_win32_handle_capacity * 2 : 16;
      struct os_win32_handle_entry *new_entries =
         realloc(os_win32_handles, new_cap * sizeof(*new_entries));
      if (!new_entries) {
         LeaveCriticalSection(&os_win32_handle_mutex);
         return -1;
      }

      os_win32_handles = new_entries;
      os_win32_handle_capacity = new_cap;
   }

   int token = InterlockedIncrement(&os_win32_next_token);
   os_win32_handles[os_win32_handle_count++] = (struct os_win32_handle_entry){
      .token = token,
      .handle = handle,
   };

   LeaveCriticalSection(&os_win32_handle_mutex);
   return token;
}

bool
os_fd_is_handle_token(int fd)
{
   os_win32_handle_ensure_init();
   EnterCriticalSection(&os_win32_handle_mutex);
   const bool found = os_win32_handle_find_index_locked(fd) >= 0;
   LeaveCriticalSection(&os_win32_handle_mutex);
   return found;
}

HANDLE
os_get_win32_handle_from_fd(int fd)
{
   intptr_t crt_handle = _get_osfhandle(fd);
   if (crt_handle != -1)
      return (HANDLE)crt_handle;

   os_win32_handle_ensure_init();
   EnterCriticalSection(&os_win32_handle_mutex);
   const ptrdiff_t idx = os_win32_handle_find_index_locked(fd);
   HANDLE handle = idx >= 0 ? os_win32_handles[idx].handle : INVALID_HANDLE_VALUE;
   LeaveCriticalSection(&os_win32_handle_mutex);
   return handle;
}

int
os_wrap_win32_handle(HANDLE handle)
{
   if (!handle || handle == INVALID_HANDLE_VALUE)
      return -1;

   return os_win32_handle_insert(handle);
}

int
os_close_fd(int fd)
{
   os_win32_handle_ensure_init();
   EnterCriticalSection(&os_win32_handle_mutex);
   const ptrdiff_t idx = os_win32_handle_find_index_locked(fd);
   if (idx >= 0) {
      HANDLE handle = os_win32_handles[idx].handle;
      os_win32_handles[idx] = os_win32_handles[os_win32_handle_count - 1];
      os_win32_handle_count--;
      LeaveCriticalSection(&os_win32_handle_mutex);
      return CloseHandle(handle) ? 0 : -1;
   }
   LeaveCriticalSection(&os_win32_handle_mutex);

   return _close(fd);
}
#else
int
os_close_fd(int fd)
{
   return close(fd);
}
#endif


FILE *
os_file_create_unique(const char *filename, int filemode)
{
   int fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, filemode);
   if (fd == -1)
      return NULL;
   return fdopen(fd, "w");
}


#if DETECT_OS_WINDOWS
int
os_dupfd_cloexec(int fd)
{
   if (os_fd_is_handle_token(fd)) {
      HANDLE handle = os_get_win32_handle_from_fd(fd);
      HANDLE dup_handle = NULL;
      if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup_handle,
                           0, FALSE, DUPLICATE_SAME_ACCESS)) {
         errno = EBADF;
         return -1;
      }

      return os_wrap_win32_handle(dup_handle);
   }

   /*
    * On Windows child processes don't inherit handles by default:
    * https://devblogs.microsoft.com/oldnewthing/20111216-00/?p=8873
    */
   return dup(fd);
}
#else
int
os_dupfd_cloexec(int fd)
{
   int minfd = 3;
   int newfd = fcntl(fd, F_DUPFD_CLOEXEC, minfd);

   if (newfd >= 0)
      return newfd;

   if (errno != EINVAL)
      return -1;

   newfd = fcntl(fd, F_DUPFD, minfd);

   if (newfd < 0)
      return -1;

   long flags = fcntl(newfd, F_GETFD);
   if (flags == -1) {
      close(newfd);
      return -1;
   }

   if (fcntl(newfd, F_SETFD, flags | FD_CLOEXEC) == -1) {
      close(newfd);
      return -1;
   }

   return newfd;
}
#endif

#include <fcntl.h>
#include <sys/stat.h>

#if DETECT_OS_WINDOWS
typedef ptrdiff_t ssize_t;
#endif

static ssize_t
readN(int fd, char *buf, size_t len)
{
   /* err was initially set to -ENODATA but in some BSD systems
    * ENODATA is not defined and ENOATTR is used instead.
    * As err is not returned by any function it can be initialized
    * to -EFAULT that exists everywhere.
    */
   int err = -EFAULT;
   size_t total = 0;
   do {
      ssize_t ret = read(fd, buf + total, len - total);

      if (ret < 0)
         ret = -errno;

      if (ret == -EINTR || ret == -EAGAIN)
         continue;

      if (ret <= 0) {
         err = ret;
         break;
      }

      total += ret;
   } while (total != len);

   return total ? (ssize_t)total : err;
}

#ifndef O_BINARY
/* Unix makes no distinction between text and binary files. */
#define O_BINARY 0
#endif

char *
os_read_file(const char *filename, size_t *size)
{
   /* Note that this also serves as a slight margin to avoid a 2x grow when
    * the file is just a few bytes larger when we read it than when we
    * fstat'ed it.
    * The string's NULL terminator is also included in here.
    */
   size_t len = 64;

   int fd = open(filename, O_RDONLY | O_BINARY);
   if (fd == -1) {
      /* errno set by open() */
      return NULL;
   }

   /* Pre-allocate a buffer at least the size of the file if we can read
    * that information.
    */
   struct stat stat;
   if (fstat(fd, &stat) == 0)
      len += stat.st_size;

   char *buf = malloc(len);
   if (!buf) {
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }

   ssize_t actually_read;
   size_t offset = 0, remaining = len - 1;
   while ((actually_read = readN(fd, buf + offset, remaining)) == (ssize_t)remaining) {
      char *newbuf = realloc(buf, 2 * len);
      if (!newbuf) {
         free(buf);
         close(fd);
         errno = -ENOMEM;
         return NULL;
      }

      buf = newbuf;
      len *= 2;
      offset += actually_read;
      remaining = len - offset - 1;
   }

   close(fd);

   if (actually_read > 0)
      offset += actually_read;

   /* Final resize to actual size */
   len = offset + 1;
   char *newbuf = realloc(buf, len);
   if (!newbuf) {
      free(buf);
      errno = -ENOMEM;
      return NULL;
   }
   buf = newbuf;

   buf[offset] = '\0';

   if (size)
      *size = offset;

   return buf;
}

#if DETECT_OS_LINUX

#include <sys/syscall.h>
#include <unistd.h>

/* copied from <linux/kcmp.h> */
#define KCMP_FILE 0

int
os_same_file_description(int fd1, int fd2)
{
   pid_t pid = getpid();

   /* Same file descriptor trivially implies same file description */
   if (fd1 == fd2)
      return 0;

   return syscall(SYS_kcmp, pid, pid, KCMP_FILE, fd1, fd2);
}

#else

int
os_same_file_description(int fd1, int fd2)
{
   /* Same file descriptor trivially implies same file description */
   if (fd1 == fd2)
      return 0;

   /* Otherwise we can't tell */
   return -1;
}

#endif
