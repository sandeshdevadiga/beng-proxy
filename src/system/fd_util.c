/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "fd_util.h"

#if !defined(_GNU_SOURCE) && (defined(HAVE_PIPE2) || defined(HAVE_ACCEPT4))
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#ifdef HAVE_INOTIFY_INIT
#include <sys/inotify.h>
#endif

#ifndef WIN32

static int
fd_mask_flags(int fd, int and_mask, int xor_mask)
{
	assert(fd >= 0);

	const int old_flags = fcntl(fd, F_GETFD, 0);
	if (old_flags < 0)
		return old_flags;

	const int new_flags = (old_flags & and_mask) ^ xor_mask;
	if (new_flags == old_flags)
		return old_flags;

	return fcntl(fd, F_SETFD, new_flags);
}

#endif /* !WIN32 */

int
fd_set_cloexec(int fd, bool enable)
{
#ifndef WIN32
	return fd_mask_flags(fd, ~FD_CLOEXEC, enable ? FD_CLOEXEC : 0);
#else
	(void)fd;
	(void)enable;

	return 0;
#endif
}

#ifndef WIN32

ssize_t
recvmsg_cloexec(int sockfd, struct msghdr *msg, int flags)
{
#ifdef MSG_CMSG_CLOEXEC
	flags |= MSG_CMSG_CLOEXEC;
#endif

	ssize_t result = recvmsg(sockfd, msg, flags);
	if (result >= 0) {
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
		while (cmsg != NULL) {
			if (cmsg->cmsg_type == SCM_RIGHTS) {
				const int *fd_p = (const int *)(const void *)
					CMSG_DATA(cmsg);
				fd_set_cloexec(*fd_p, true);
			}

			cmsg = CMSG_NXTHDR(msg, cmsg);
		}
	}

	return result;
}

#endif
