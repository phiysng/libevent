/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;
	int nfds;
	struct epoll_event *events;
	int nevents;
	int epfd;
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

#define NEVENT	32000

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

/* epoll ctor */
static void *
epoll_init(struct event_base *base)
{
	int epfd, nfiles = NEVENT;
	struct rlimit rl;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (getenv("EVENT_NOEPOLL"))
		return (NULL);

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
	    rl.rlim_cur != RLIM_INFINITY) {
		/*
		 * Solaris is somewhat retarded - it's important to drop
		 * backwards compatibility when making changes.  So, don't
		 * dare to put rl.rlim_cur here.
		 */
		nfiles = rl.rlim_cur - 1;
	}

	/* Initalize the kernel queue */
	/* 创建epoll实例 */
	if ((epfd = epoll_create(nfiles)) == -1) {
		if (errno != ENOSYS)
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);
	// 存储数据成员
	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */
	epollop->events = malloc(nfiles * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	epollop->nevents = nfiles;
	// 存储读/写事件指针
	epollop->fds = calloc(nfiles, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	epollop->nfds = nfiles;
	//初始化信号句柄
	evsignal_init(base);

	return (epollop);
}

static int
epoll_recalc(struct event_base *base, void *arg, int max)
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;
		// 清空新分配的内存
		// evread == evwrite == NULL
		memset(fds + epollop->nfds, 0,
		    (nfds - epollop->nfds) * sizeof(struct evepoll));
		epollop->nfds = nfds;
	}

	return (0);
}
/**
 * @brief poll with epoll backend
 * 
 * @param base corresponding event_base.
 * @param arg data used by epollops
 * @param tv timeout time.
 * @return int 0 on success , -1 on failure.
 */
static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)
{
	struct epollop *epollop = arg;
	//所有的监听的事件
	struct epoll_event *events = epollop->events;
	struct evepoll *evep;
	int i, res, timeout = -1;

	if (tv != NULL)
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}
		/* interrupted by signal. */
		/* errno == EINT epoll syscall被信号终端 */
		evsignal_process(base);
		return (0);
	} else if (base->sig.evsignal_caught) {
		// 捕获了信号
		evsignal_process(base);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
	/**
	 * 遍历获得的res个事件
	 */
	for (i = 0; i < res; i++) {
		int what = events[i].events;
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;

		if (fd < 0 || fd >= epollop->nfds)
			continue;
		evep = &epollop->fds[fd];

		// EPOLLHUP EPOLLERR时激活读写事件
		// 读写都可以探测出对端关闭/出错
		if (what & (EPOLLHUP|EPOLLERR)) {
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			// readable event.
			if (what & EPOLLIN) {
				evread = evep->evread;
			}
			// writeable event
			// 读写事件不互斥
			if (what & EPOLLOUT) {
				evwrite = evep->evwrite;
			}
		}
		// TODO: defensive programming? or it is possible to be NEITHER Readable Writeable,HUP and ERR?
		
		if (!(evread||evwrite))
			continue;
		/**
		 * 添加到 读/写 队列
		 */ 
		if (evread != NULL)
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	return (0);
}


static int
epoll_add(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;
	/* 将信号整合进 io multiplexing接口 */
	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(ev));

	fd = ev->ev_fd;
	// fd无法被当前[0,nfds)包含 则需要扩容
	if (fd >= epollop->nfds) {
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
			return (-1);
	}
	evep = &epollop->fds[fd];
	op = EPOLL_CTL_ADD;
	events = 0;
	/**
	 * evep->evread != NULL 意味着这个事件已经注册到了此epoll instance
	 * op 需要设为 EPOLL_CTL_MOD
	 */
	if (evep->evread != NULL) {
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}
	/* 抽取 interest events */
	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	epev.data.fd = fd;
	epev.events = events;
	/* epoll interface , do real work */
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)
			return (-1);

	/* Update events responsible */
	/* 
	 * 更新fds[]所维护的 fd->(evread : event , evwrite : event)之间的映射
	 * TODO: 这个实现似乎是因为epoll不允许捎带数据 参照kqueue验证自己的想法
	 * 无则新增 有则更新
	 */
	if (ev->ev_events & EV_READ)
		evep->evread = ev;
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev;

	return (0);
}

static int
epoll_del(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_del(ev));

	fd = ev->ev_fd;
	// fd not registered.
	if (fd >= epollop->nfds)
		return (0);
	evep = &epollop->fds[fd];
	
	/* 注意默认是 DEL */
	op = EPOLL_CTL_DEL;
	events = 0;
	/* 抽取准备删除的事件 */
	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;
	/* event是 读/写事件之一 应该不会什么也没有 */

	/**
	 * 如果事件存在R&W事件 且删除其中一个事件 则调用 CTL
	 * 否则(存在R&W && 删除全部两个事件 || 存在一个事件(R || W ) 删除这个事件) 此时调用DEL
	 * 
	 */
	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
		/* 只删除读事件 且写事件也存在 
		 * evep->evwrite != NULL -> 写事件存在
		 * 此时调用 EPOLL_CTL_MOD 将原来的事件覆盖为EPOLLOUT
		 * 
		 * 对应的 只删除写事件 也是将原来的事件覆盖为EPOLLIN
		 */
		if ((events & EPOLLIN) && evep->evwrite != NULL) {
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} else if ((events & EPOLLOUT) && evep->evread != NULL) {
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;

	/* clear corresponding map */
	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;

	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
		return (-1);

	return (0);
}

/* epoll dtor */
static void
epoll_dealloc(struct event_base *base, void *arg)
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);
	if (epollop->fds)
		free(epollop->fds);
	if (epollop->events)
		free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
