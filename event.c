/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

// 全局变量作为标志
static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	return (evutil_gettimeofday(tp, NULL));
}

/*
 * init event_base base.
 * call event_base_new inside.
 */
struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;
	// allocate memory.
	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	event_sigcb = NULL;
	event_gotsig = 0;

	detect_monotonic();
	gettime(base, &base->event_tv);
	
	// timer min_pq.
	min_heap_ctor(&base->timeheap);
	//初始化事件队列
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;
	
	/* 选择IO多路复用机制
	 * Linux->epoll
	 * FreeBSD->kqueue
 	 */
	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	/* 只分配一个活跃事件队列 */
	event_base_priority_init(base, 1);

	return (base);
}

/* TODO: to be read. */
void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;
	}
	
	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;
	/* 已经存在活跃的事件 则直接错误退出
	 * 应该是因为后面的操作有可能会销毁现存的队列再创建
	 */
	if (base->event_count_active)
		return (-1);
	/**
	 * 如果存在激活队列且 队列数量不等于 npriorities
	 * 释放之前分配的队列
	 * activequeues **指针 保存*指针
	 * activequeues[i] *指针
	 */ 
	if (base->nactivequeues && npriorities != base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	// activequeues数组保存指针,指针指向event_list.
	base->activequeues = (struct event_list **)calloc(base->nactivequeues,
	    npriorities * sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	/*
	 * 分配(1-n)个队列所占的空间
	 */
	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */
/**
 * 所谓的优先级队列(就是n个队列 每个队列中储存优先级相同的事件)
 * 有可能存在多个队列 优先级不同
 * 总是先执行数字低(优先级高)的事件 上面的注释意思其实有点迷
 * 优先级低的进程有可能会长时间得不到处理
 * 
 * TODO: 注意区分优先级 与 指代优先级的数量 
 * 优先级的数字越小 优先级越高
 */ 
static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;
	// 获得当前最靠前的非空队列 <- 优先级最高
	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	// 此时应该存在非空的活跃事件队列
	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		// PERSIST事件依然继续监听 只从AVTIVE QUEUE移除
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;

		// 调用回调函数
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (event_gotsig || base->event_break)
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

/**
 * get backend of your platform.
 */ 
const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	if (base->sig.ev_signal_added)
		evsignal_base = base;
	done = 0;
	while (!done) {
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}
		// the first time we go to there we need to call gettime one time for 
		// there is no cached time available.
		
		// tv indicate time when last poll completed thanks to the cached_time in base.
		timeout_correct(base, &tv);

		tv_p = &tv;
		
		/**
		 * @brief 
		 * 没有事件被触发的时候 计算下一次的超时时间
		 * 注意这里如果 EVLOOP_NONBLOCK被设置
		 * 那么肯定会导致tv被设置为0 sec.
		 * epoll等会立即返回(此时如果没有事件出现 则会退出此事件循环)
		 * 
		 * Note:
		 * 在此时可能存在活跃的未处理的事件 这可能是上一次 `poll`的时候未处理的低优先级事件
		 * libevent在一次循环中可能只会执行优先级高的回调函数,其余的低优先级的留待以后执行
		 */
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			timeout_next(base, &tv_p);
		} else {
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);
		}
		
		/* If we have no events, we just exit */
		/* 没有事件出现 直接返回 */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
		/* 将上一次获得的时间赋给 event_tv */
		gettime(base, &base->event_tv);

		/* clear time cache */
		/* 清空时间缓存 下一次call gettime()需要调用syscall */
		base->tv_cache.tv_sec = 0;

		// call for ready io events , like epoll/kqueue etc.
		res = evsel->dispatch(base, evbase, tv_p);

		// error occured.
		if (res == -1)
			return (-1);

		// 更新缓存的时间 这个时间用于接下来下一次循环的时候获得时间 即tag getime
		// 以及接下来用于处理超时事件
		gettime(base, &base->tv_cache);

		// checkout ready timer and move them to the ready queue.
		timeout_process(base);

		if (base->event_count_active) {
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			/* libevent在NON-BLOCK模式下 没有活跃事件会立即退出事件循环 */
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	/* 我们无法支持只激活一次的信号 */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;
	/* timer默认只激活一次 */
	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	/* 默认分配current_base
	 * 调用者需要去设置其真正想设置的base.
	 */
	ev->ev_base = current_base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}

int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	/* 只有`纯洁`的ev会被分配给一个不同的event_base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 * 
 * 更改ev的pri,如果ev已经被调度,更改优先级会失败
 */

int
event_priority_set(struct event *ev, int pri)
{
	/* ev is running , so it shall fail */
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	/* invalid priority */
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *base = ev->ev_base;
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	/*
	 * 为下面的超时插入做准备, 如果我们在任何一个步骤失败,那么不应该改变库的
	 * 任何状态
	 */ 
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}
	
	/**
	 * events shall in EV_READ|EV_WRITE|EV_SIGNAL
	 * and ev shall not in EVLIST_INSERTED|EVLIST_ACTIVE
	 * 即ev待注册的信号有效 且 ev没有被注册过
	 */
	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		// register in epoll.
		res = evsel->add(evbase, ev);
		// add to event queue.
		if (res != -1)
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 * 我们只会在上面的步骤都成功才更改超时状态(min_pq)
	 */
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 * 我们已经在上面预留min_pq的内存(位置)了
		 * 
		 * 删除事件如果事件存在于超时队列中
		 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. 
		 * 检查是否因为超时这个ev被放入活跃队列.
		 * 在回调函数可以被执行之前重新调度这个超时
		 * 从活跃队列删除ev.
		 * */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		gettime(base, &now);
		/* calculate timeout time from now on. */
		evutil_timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));
		/* 加入超时队列 等待超时事件被触发 */
		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}

	return (res);
}

int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	/* 看看我们是否正在一个循环里活跃执行这个事件 */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	}

	return (0);
}

void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	/* 对于已经激活的ev , 将res -> ev_res */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;
	
	// 获取此时最可能超时的定时器 以及 错误处理
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}
	// 获取的是缓存的时间
	if (gettime(base, &now) == -1)
		return (-1);

	// 此定时器此时已经超时了 会将tv 即poll timeout 设为 0, poll会立即返回 然后处理超时事件
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}
	// 定时器此次循环不会超时 
	// 此时计算我们需要传给 `epoll etc`的超时时间
	// 堆顶event的超时时间 - now
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	/* will use cached time , the cached time is the time when last poll is completed 
	 * this time will be used to calculate the timeout of `epoll etc`.
	 */
	gettime(base, tv);
	// tv is the time last poll completed.
	// if the event time <= current time ,just return.
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));

	// 原因一般为用户将系统时间向前调整了
	// 获得base时间与tv获得的系统时间相比快了多少
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	/**
	 * 现在base的时间比系统时间快,此时定时器的时间过长了
	 * 比如base 14:00 timer 10min
	 * gettime() 获得时间 13:55 那么此时我们需要将时间减去(14:00 - 13:55) = 5min
	 * 即 timer调整为 5min
	 * 小/大顶堆同时增减 value x,堆依然有效
	 */ 
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	/* 更新当前的系统时间 */
	base->event_tv = *tv;
}

void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;
	//没有定时器
	if (min_heap_empty(&base->timeheap))
		return;
	// 此次poll完成的时间
	gettime(base, &now);

	//超时事件在堆顶 循环直到堆顶大于当前时间
	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		// 增加此定时器超时事件
		event_active(ev, EV_TIMEOUT, 1);
	}
}

void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;
	// 从ev_flags删除待删除的queue flag.
	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/**
 * @brief 添加ev至指定的队列
 * 
 * @param base event_base
 * @param ev event to be added
 * @param queue specified queue.
 */
void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		/* 对于激活队列 允许其再次插入同一个事件 
		 * FIXME: really🐶? just return back. -_-.
		 */
		if (queue & EVLIST_ACTIVE)
			return;
		/* report error on second insert */
		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	// update flags to show that ev is in this queue(s).
	ev->ev_flags |= queue;
	/**
	 * libevent内部存在
	 * - 包含所有ev的队列
	 * - 激活队列: 完成IO的event,timeout timer ,etc.
	 * - 超时队列: 存在超时时间的队列
	 */  
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		// increase event count.
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {
		// push to min_pq.
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
