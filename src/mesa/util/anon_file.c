/*
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Based on weston shared/os-compatibility.c
 */

#include "anon_file.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#if defined(HAVE_MEMFD_CREATE) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/mman.h>
#elif defined(__ANDROID__)
#include <sys/syscall.h>
#include <linux/memfd.h>
#else
#include <stdio.h>
#endif
#endif /* _WIN32 */

#ifndef _WIN32
#if !(defined(__FreeBSD__) || defined(HAVE_MEMFD_CREATE) || defined(HAVE_MKOSTEMP) || defined(__ANDROID__))
static int
set_cloexec_or_close(int fd)
{
   long flags;

   if (fd == -1)
      return -1;

   flags = fcntl(fd, F_GETFD);
   if (flags == -1)
      goto err;

   if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
      goto err;

   return fd;

err:
   close(fd);
   return -1;
}
#endif

#if !(defined(__FreeBSD__) || defined(HAVE_MEMFD_CREATE) || defined(__ANDROID__))
static int
create_tmpfile_cloexec(char *tmpname)
{
   int fd;

#ifdef HAVE_MKOSTEMP
   fd = mkostemp(tmpname, O_CLOEXEC);
#else
   fd = mkstemp(tmpname);
#endif

   if (fd < 0) {
      return fd;
   }

#ifndef HAVE_MKOSTEMP
   fd = set_cloexec_or_close(fd);
#endif

   unlink(tmpname);
   return fd;
}
#endif

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * An optional name for debugging can be provided as the second argument.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * If memfd or SHM_ANON is supported, the filesystem is not touched at all.
 * Otherwise, the file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 */
#endif /* !_WIN32 - close guard around helper functions */

#ifdef _WIN32
int
os_create_anonymous_file(off_t size, const char *debug_name)
{
   char temp_path[MAX_PATH];
   char temp_file[MAX_PATH];

   DWORD path_len = GetTempPathA(MAX_PATH, temp_path);
   if (!path_len || path_len >= MAX_PATH)
      return -1;

   UINT name_ret = GetTempFileNameA(temp_path,
                                    (debug_name && debug_name[0]) ? debug_name : "vrg",
                                    0, temp_file);
   if (!name_ret)
      return -1;

   HANDLE file = CreateFileA(temp_file,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                             NULL);
   if (file == INVALID_HANDLE_VALUE) {
      DeleteFileA(temp_file);
      return -1;
   }

   LARGE_INTEGER file_size;
   file_size.QuadPart = size;
   if (!SetFilePointerEx(file, file_size, NULL, FILE_BEGIN) ||
       !SetEndOfFile(file)) {
      CloseHandle(file);
      DeleteFileA(temp_file);
      return -1;
   }

   int fd = _open_osfhandle((intptr_t)file, _O_BINARY);
   if (fd < 0) {
      CloseHandle(file);
      DeleteFileA(temp_file);
      return -1;
   }

   return fd;
}
#else /* !_WIN32 */
int
os_create_anonymous_file(off_t size, const char *debug_name)
{
   int fd, ret;
#if defined(HAVE_MEMFD_CREATE)
   if (!debug_name)
      debug_name = "mesa-shared";
   fd = memfd_create(debug_name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
#elif defined(__ANDROID__)
   if (!debug_name)
      debug_name = "mesa-shared";
   fd = syscall(SYS_memfd_create, debug_name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
#elif defined(__FreeBSD__)
   fd = shm_open(SHM_ANON, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
#elif defined(__OpenBSD__)
   char template[] = "/tmp/mesa-XXXXXXXXXX";
   fd = shm_mkstemp(template);
   if (fd != -1)
      shm_unlink(template);
#else
   const char *path;
   char *name;

   path = getenv("XDG_RUNTIME_DIR");
   if (!path)
      path = "/tmp";

   if (debug_name)
      asprintf(&name, "%s/mesa-shared-%s-XXXXXX", path, debug_name);
   else
      asprintf(&name, "%s/mesa-shared-XXXXXX", path);
   if (!name)
      return -1;

   fd = create_tmpfile_cloexec(name);

   free(name);
#endif

   if (fd < 0)
      return -1;

   ret = ftruncate(fd, size);
   if (ret < 0) {
      close(fd);
      return -1;
   }

   return fd;
}
#endif /* !_WIN32 */
