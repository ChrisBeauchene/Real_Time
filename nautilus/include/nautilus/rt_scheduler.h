//
//  rt_scheduler.h
//  rt_scheduler_test
//
//  Created by Chris Beauchene on 2/15/16.
//  Copyright © 2016 EECS 395/495 Kernel Development. All rights reserved.
//

#ifndef rt_scheduler_h
#define rt_scheduler_h

#include <nautilus/thread.h>

/******************************************************************
 REAL TIME THREAD
 THREE TYPES (all have their own constraints)
 - PERIODIC
 - SPORADIC
 - APERIODIC
 
 THREE STATUSES
 - CREATED
 - ADMITTED
 - RUNNING
 
 ******************************************************************/

struct periodic_constraints {
    uint64_t period, slice;
};

struct sporadic_constraints {
    uint64_t work;
};

struct aperiodic_constraints {
    uint64_t priority;
};

typedef union rt_constraints {
    struct periodic_constraints     periodic;
    struct sporadic_constraints     sporadic;
    struct aperiodic_constraints    aperiodic;
} rt_constraints;

typedef enum { APERIODIC = 0, SPORADIC = 1, PERIODIC = 2} rt_type;

typedef enum {  RUNNABLE_QUEUE = 0, PENDING_QUEUE = 1, 
                APERIODIC_QUEUE = 2, ARRIVAL_QUEUE = 3,
                WAITING_QUEUE = 4, SLEEPING_QUEUE = 5, EXITED_QUEUE = 6} queue_type;

typedef enum {  ARRIVED = 0, ADMITTED = 1, WAITING = 2, 
                RUNNING = 3, TOBE_REMOVED = 4, REMOVED = 5, 
                SLEEPING = 6} rt_status;

typedef struct rt_thread {
    rt_type type;
    queue_type q_type;
    rt_status status;
    rt_constraints *constraints;
    uint64_t start_time; 
    uint64_t run_time;
    uint64_t deadline;
    uint64_t exit_time;
    struct nk_thread *thread;
} rt_thread;

rt_thread* rt_thread_init(int type,
                          rt_constraints *constraints,
                          uint64_t deadline,
                          struct nk_thread *thread
                          );

typedef struct rt_queue {
    queue_type type;
    uint64_t size, head, tail;
    rt_thread *threads[0];
} rt_queue ;

typedef struct tsc_info {
    uint64_t set_time;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t elapsed_time;
    uint64_t error;
} tsc_info;

typedef struct rt_scheduler {
    rt_queue *runnable;
    rt_queue *pending;
    rt_queue *aperiodic;
    rt_queue *arrival;
    rt_queue *waiting;
    rt_queue *exited;
    rt_queue *sleeping;
    rt_queue *trash;
    rt_thread *main_thread;
    uint64_t run_time;
    tsc_info *tsc;
} rt_scheduler;

rt_scheduler* rt_scheduler_init(rt_thread *main_thread);
struct nk_thread* rt_need_resched();
void rt_start(uint64_t sched_slice_time, uint64_t sched_period);

void enqueue_thread(rt_queue *queue, rt_thread *thread);
rt_thread* dequeue_thread(rt_queue *queue);
rt_thread* remove_thread(rt_thread *thread);
int rt_thread_exit(rt_thread *thread);
void rt_thread_free(rt_thread *thread);
void rt_thread_dump(rt_thread *thread);

// Time
uint64_t cur_time();

/*
 nk_thread_t * nk_rt_need_resched();
 */

/* ADMISSION CONTROL */



int rt_admit(rt_scheduler *scheduler, rt_thread *thread);



#endif /* rt_scheduler_h */
