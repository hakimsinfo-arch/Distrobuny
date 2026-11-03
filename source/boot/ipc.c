/*
 * Distro Buny — runtime IPC between buny-init and buny-ui
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot/ipc.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int ipc_send(int sock, const struct ipc_msg *m, const int *fds, int nfds)
{
	struct iovec iov = { (void *)m, sizeof(*m) };
	union {
		char buf[CMSG_SPACE(sizeof(int) * BUNY_IPC_MAX_FDS)];
		struct cmsghdr align;
	} u;
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	if (nfds > 0) {
		if (nfds > BUNY_IPC_MAX_FDS) {
			errno = EINVAL;
			return -1;
		}
		memset(&u, 0, sizeof(u));
		mh.msg_control = u.buf;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
		memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
	}
	ssize_t r;
	do
		r = sendmsg(sock, &mh, MSG_NOSIGNAL);
	while (r < 0 && errno == EINTR);
	return r == (ssize_t)sizeof(*m) ? 0 : -1;
}

int ipc_recv(int sock, struct ipc_msg *m, int *fds, int *nfds)
{
	struct iovec iov = { m, sizeof(*m) };
	union {
		char buf[CMSG_SPACE(sizeof(int) * BUNY_IPC_MAX_FDS)];
		struct cmsghdr align;
	} u;
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = u.buf;
	mh.msg_controllen = sizeof(u.buf);
	ssize_t r;
	do
		r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
	while (r < 0 && errno == EINTR);
	if (nfds)
		*nfds = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;
		int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		int got[BUNY_IPC_MAX_FDS + 4];
		if (n > (int)(sizeof(got) / sizeof(got[0])))
			n = (int)(sizeof(got) / sizeof(got[0]));
		memcpy(got, CMSG_DATA(c), sizeof(int) * (size_t)n);
		for (int i = 0; i < n; i++) {
			if (fds && nfds && *nfds < BUNY_IPC_MAX_FDS)
				fds[(*nfds)++] = got[i];
			else
				close(got[i]);
		}
	}
	if (r != (ssize_t)sizeof(*m) || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
		for (int i = 0; fds && nfds && i < *nfds; i++)
			close(fds[i]);
		if (nfds)
			*nfds = 0;
		if (r >= 0)
			errno = EPROTO;
		return -1;
	}
	/* never trust lengths: force termination of every string field */
	m->path[sizeof(m->path) - 1] = '\0';
	m->label[sizeof(m->label) - 1] = '\0';
	m->text[sizeof(m->text) - 1] = '\0';
	return 0;
}
