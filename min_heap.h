/*
 * Copyright (c) 2006 Maxim Yegorushkin <maxim.yegorushkin@gmail.com>
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
#ifndef _MIN_HEAP_H_
#define _MIN_HEAP_H_

#include "event.h"
#include "evutil.h"

typedef struct min_heap
{
    struct event** p;
    unsigned n, a;
} min_heap_t;

static inline void           min_heap_ctor(min_heap_t* s);
static inline void           min_heap_dtor(min_heap_t* s);
static inline void           min_heap_elem_init(struct event* e);
static inline int            min_heap_elem_greater(struct event *a, struct event *b);
static inline int            min_heap_empty(min_heap_t* s);
static inline unsigned       min_heap_size(min_heap_t* s);
static inline struct event*  min_heap_top(min_heap_t* s);
static inline int            min_heap_reserve(min_heap_t* s, unsigned n);
static inline int            min_heap_push(min_heap_t* s, struct event* e);
static inline struct event*  min_heap_pop(min_heap_t* s);
static inline int            min_heap_erase(min_heap_t* s, struct event* e);
static inline void           min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void           min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e);

int min_heap_elem_greater(struct event *a, struct event *b)
{
    return evutil_timercmp(&a->ev_timeout, &b->ev_timeout, >);
}

void min_heap_ctor(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }
void min_heap_dtor(min_heap_t* s) { free(s->p); }
void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }
int min_heap_empty(min_heap_t* s) { return 0u == s->n; }
unsigned min_heap_size(min_heap_t* s) { return s->n; }
struct event* min_heap_top(min_heap_t* s) { return s->n ? *s->p : 0; }

int min_heap_push(min_heap_t* s, struct event* e)
{
    // check capacity.
        if(min_heap_reserve(s, s->n + 1))
        return -1;
    // now we have enough space.
    // the index is [0,n-1] if cap is n.
    // hence the value of n++ ( actually n) is the new index.
    // now what we need to do is swim up.
    min_heap_shift_up_(s, s->n++, e);
    return 0;
}

struct event* min_heap_pop(min_heap_t* s)
{
    if(s->n)
    {
        struct event* e = *s->p;
        // reduce cap by one.
        // turn the last item to the pq top and sink to a proper position.
        min_heap_shift_down_(s, 0u, s->p[--s->n]);
        e->min_heap_idx = -1;
        return e;
    }
    return 0;
}

int min_heap_erase(min_heap_t* s, struct event* e)
{
    if(((unsigned int)-1) != e->min_heap_idx)
    {
        struct event *last = s->p[--s->n];
        unsigned parent = (e->min_heap_idx - 1) / 2;
	/* we replace e with the last element in the heap.  We might need to
	   shift it upward if it is less than its parent, or downward if it is
	   greater than one or both its children. Since the children are known
	   to be less than the parent, it can't need to shift both up and
	   down. */
        if (e->min_heap_idx > 0 && min_heap_elem_greater(s->p[parent], last))
             min_heap_shift_up_(s, e->min_heap_idx, last);
        else
             min_heap_shift_down_(s, e->min_heap_idx, last);
        e->min_heap_idx = -1;
        return 0;
    }
    return -1;
}

int min_heap_reserve(min_heap_t* s, unsigned n)
{
    if(s->a < n)
    {
        struct event** p;
        // heap preallocated size is 8.
        // a is the capacity prereserved.
        unsigned a = s->a ? s->a * 2 : 8;
        if(a < n)
            a = n;
        if(!(p = (struct event**)realloc(s->p, a * sizeof *p)))
            return -1;
        s->p = p;
        s->a = a;
    }
    return 0;
}

void min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    /**
     * 1 / 2 == 0 & 2 / 2 == 1
     * however node 1 and 2 have the same parent node.
     * (0 and 1) / 2 == 0 , this works properly.
     */
    unsigned parent = (hole_index - 1) / 2;
    
    // 因为这里将待插入的数放在后面,所以使用 `>`
    // 一般的实现,如`priority_queue` in c++ use less<> as the operator of min pq.
    while(hole_index && min_heap_elem_greater(s->p[parent], e))
    {
        // move the parent to the child position and update the index
        // keep the event e until its parent is less than itself.
        (s->p[hole_index] = s->p[parent])->min_heap_idx = hole_index;
        // to the upper level.
        hole_index = parent;
        parent = (hole_index - 1) / 2;
    }
    // now the parent is lesser one.
    // assign to the current position whick is fact the preserve the previous parent value.
    // textbook implement.
    (s->p[hole_index] = e)->min_heap_idx = hole_index;
}

void min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    // 右子节点的index
    // should point to the smallest child node.
    unsigned min_child = 2 * (hole_index + 1);

    // example. n = 5
    //       0 
    //      / |
    //      1 2
    //     /| | 
    //    3 4 5
    while(min_child <= s->n)
	{
        // min_child == s->n indicate that the right child doesn't exists.
        // hence we just need to check the left child.
        // or we need to pick up the smaller one to be compared.
        // saying if right node > left node , we need to minus one.
        min_child -= min_child == s->n || min_heap_elem_greater(s->p[min_child], s->p[min_child - 1]);
        // if e <= smallest child , the heap is in order.
        if(!(min_heap_elem_greater(e, s->p[min_child])))
            break;
        // else move the child to hole_index
        // similar to min_heap_shift_up_
        (s->p[hole_index] = s->p[min_child])->min_heap_idx = hole_index;
        hole_index = min_child;
        min_child = 2 * (hole_index + 1);
	}
    // note that when we break the loop , we didn't put the new node e in proper position.
    // this function call just assign event e to its position.
    // the function will stop the first time it run to the while loop as the FALSE is doomed.
    min_heap_shift_up_(s, hole_index,  e);
}

#endif /* _MIN_HEAP_H_ */
