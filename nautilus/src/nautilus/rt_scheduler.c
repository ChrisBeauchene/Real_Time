

//  rt_scheduler.c
//  rt_scheduler_test
//
//  Created by Chris Beauchene on 2/15/16.
//  Copyright © 2016 EECS 395/495 Kernel Development. All rights reserved.
//
//



#include <nautilus/nautilus.h>
#include <nautilus/thread.h>
#include <nautilus/rt_scheduler.h>
#include <nautilus/irq.h>
#include <nautilus/cpu.h>
#include <nautilus/cpuid.h>
#include <dev/apic.h>
#include <dev/timer.h>


#define INFO(fmt, args...) printk("RT SCHED: " fmt, ##args)
#define RT_SCHED_PRINT(fmt, args...) printk("RT SCHED: " fmt, ##args)
#define RT_SCHED_ERROR(fmt, args...) printk("RT SCHED ERROR: " fmt, ##args)

#define RT_SCHED_DEBUG(fmt, args...)
#ifdef NAUT_CONFIG_DEBUG_RT_SCHEDULER
#undef RT_SCHED_DEBUG
#define RT_SCHED_DEBUG(fmt, args...) printk("RT SCHED: " fmt, ##args)
#endif

#define parent(i) ((i) ? (((i) - 1) >> 1) : 0)
#define left_child(i) (((i) << 1) + 1)
#define right_child(i) (((i) << 1) + 2)

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) >(y)) ? (x) : (y))
#endif

// Thread definitions
// Types
#define APERIODIC 0
#define SPORADIC 1
#define PERIODIC 2

// UTILIZATION FACTOR LIMITS
#define PERIODIC_UTIL 65000
#define SPORADIC_UTIL 18000
#define APERIODIC_UTIL 9000

#define ARRIVED 0
#define ADMITTED 1
#define WAITING 2
#define RUNNING 3
#define TOBE_REMOVED 4
#define REMOVED 5
#define SLEEPING 6

// Queue types
#define RUNNABLE_QUEUE 0
#define PENDING_QUEUE 1
#define APERIODIC_QUEUE 2
#define ARRIVAL_QUEUE 3
#define WAITING_QUEUE 4
#define SLEEPING_QUEUE 5
#define EXITED_QUEUE 6
#define MAX_QUEUE 256

#define QUANTUM 10000000

typedef struct rt_thread_sim {
    rt_type type;
    queue_type q_type;
    rt_status status;
    rt_constraints *constraints;
    uint64_t start_time; 
    uint64_t run_time;
    uint64_t deadline;
    uint64_t exit_time;
} rt_thread_sim;

typedef struct rt_queue_sim {
    queue_type type;
    uint64_t size, head, tail;
    rt_thread_sim *threads[0];
} rt_queue_sim;

typedef struct rt_simulator {
    rt_queue_sim *runnable;
    rt_queue_sim *pending;
    rt_queue_sim *aperiodic;
} rt_simulator;

static rt_simulator* init_simulator();

// Switching thread function
static int check_deadlines(rt_thread *t);
static inline void update_periodic(rt_thread *t);
static void set_timer(rt_scheduler *scheduler, rt_thread *thread, uint64_t end_time, uint64_t slack);

// Admission Control Functions
static rt_thread_sim *rt_need_resched_logic(rt_simulator *simulator, rt_thread_sim *thread, uint64_t time);
static uint64_t set_timer_logic(rt_simulator *simulator, rt_thread_sim *thread, uint64_t time);
static void enqueue_thread_logic(rt_queue_sim *queue, rt_thread_sim *thread);
static rt_thread_sim* dequeue_thread_logic(rt_queue_sim *queue);
static inline void update_exit_logic(rt_thread_sim *t, uint64_t time);
static inline void update_enter_logic(rt_thread_sim *t, uint64_t time);
static int check_deadlines_logic(rt_thread_sim *t, uint64_t time);
static inline void update_periodic_logic(rt_thread_sim *t, uint64_t time);
static void copy_threads_sim(rt_simulator *simulator, rt_scheduler *scheduler);
static void free_threads_sim(rt_simulator *simulator);

static rt_thread* max_periodic(rt_scheduler *scheduler);

static inline uint64_t get_min_per(rt_queue *runnable, rt_queue *queue, rt_thread *thread);
static inline uint64_t get_avg_per(rt_queue *runnable, rt_queue *pending, rt_thread *thread);
static inline uint64_t get_per_util(rt_queue *runnable, rt_queue *pending);
static inline uint64_t get_spor_util(rt_queue *runnable);
static inline uint64_t umin(uint64_t x, uint64_t y);
/********** Function definitions *************/


// SCHEDULE FUNCTIONS
static void sched_sim(void *scheduler);

rt_thread* rt_thread_init(int type,
                          rt_constraints *constraints,
                          uint64_t deadline,
                          struct nk_thread *thread
                          )
{
    rt_thread *t = (rt_thread *)malloc(sizeof(rt_thread));
    t->type = type;
    t->status = ARRIVED;
    t->constraints = constraints;
    t->start_time = 0;
    t->run_time = 0;
    t->deadline = 0;

    if (type == PERIODIC)
    {
        t->deadline = cur_time() + constraints->periodic.period;
    } else if (type == SPORADIC)
    {
        t->deadline = cur_time() + deadline;
    }
    
    thread->rt_thread = t;
    t->thread = thread;
    return t;
}

rt_scheduler* rt_scheduler_init(rt_thread *main_thread)
{
    rt_scheduler* scheduler = (rt_scheduler *)malloc(sizeof(rt_scheduler));
    rt_queue *runnable = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *pending = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *aperiodic = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *arrival = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *waiting = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *sleeping = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *exited = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));
    rt_queue *trash = (rt_queue *)malloc(sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *));

    tsc_info *info = (tsc_info *)malloc(sizeof(tsc_info));
	main_thread->status = ADMITTED;
	scheduler->main_thread = main_thread;

    if (!scheduler || !runnable || ! pending || !aperiodic || !arrival || !waiting || !info || !sleeping || !exited || !trash) {
        RT_SCHED_ERROR("Could not allocate rt scheduler\n");
        return NULL;
    } else {
#define ZERO(x) memset(x, 0, sizeof(*x))
#define ZERO_QUEUE(x) memset(x, 0, sizeof(rt_queue) + MAX_QUEUE * sizeof(rt_thread *))

		ZERO(scheduler);
		ZERO(info);
		ZERO_QUEUE(runnable);
		ZERO_QUEUE(pending);
		ZERO_QUEUE(aperiodic);
		ZERO_QUEUE(arrival);
		ZERO_QUEUE(waiting);
        ZERO_QUEUE(sleeping);
        ZERO_QUEUE(exited);
        ZERO_QUEUE(trash);
		
        runnable->type = RUNNABLE_QUEUE;
        runnable->size = 0;
        scheduler->runnable = runnable;
        
        pending->type = PENDING_QUEUE;
        pending->size = 0;
        scheduler->pending = pending;
        
        aperiodic->type = APERIODIC_QUEUE;
        aperiodic->size = 0;
        scheduler->aperiodic = aperiodic;

        arrival->type = ARRIVAL_QUEUE;
        arrival->size = 0;
        arrival->head = 0;
        arrival->tail = 0;
        scheduler->arrival = arrival;

        waiting->type = WAITING_QUEUE;
        waiting->size = 0;
        waiting->head = 0;
        waiting->tail = 0;
        scheduler->waiting = waiting;
		
        sleeping->type = SLEEPING_QUEUE;
		sleeping->size = 0;
        sleeping->head = 0;
        sleeping->tail = 0;
        scheduler->sleeping = sleeping;

        exited->type = EXITED_QUEUE;
        exited->size = 0;
        exited->head = 0;
        exited->tail = 0;
        scheduler->exited = exited;

        trash->size = 0;
        trash->head = 0;
        trash->tail = 0;
        scheduler->trash = trash;

        scheduler->tsc = info;

    }

    enqueue_thread(aperiodic, main_thread);
    return scheduler;
}

static rt_simulator* init_simulator() {
    rt_simulator* simulator = (rt_simulator *)malloc(sizeof(rt_simulator));
    rt_queue_sim *runnable = (rt_queue_sim *)malloc(sizeof(rt_queue_sim) + MAX_QUEUE * sizeof(rt_thread_sim *));
    rt_queue_sim *pending = (rt_queue_sim *)malloc(sizeof(rt_queue_sim) + MAX_QUEUE * sizeof(rt_thread_sim *));
    rt_queue_sim *aperiodic = (rt_queue_sim *)malloc(sizeof(rt_queue_sim) + MAX_QUEUE * sizeof(rt_thread_sim *));

    if (!simulator || !runnable || ! pending || !aperiodic) {
        RT_SCHED_ERROR("Could not allocate rt simulator\n");
        return NULL;
    } else {
        runnable->type = RUNNABLE_QUEUE;
        runnable->size = 0;
        simulator->runnable = runnable;
        
        pending->type = PENDING_QUEUE;
        pending->size = 0;
        simulator->pending = pending;
        
        aperiodic->type = APERIODIC_QUEUE;
        aperiodic->size = 0;
        simulator->aperiodic = aperiodic;
    }
    return simulator;
}


void enqueue_thread(rt_queue *queue, rt_thread *thread)
{
    if (queue->type == RUNNABLE_QUEUE)
    {
        if (queue->size == MAX_QUEUE)
        {
            RT_SCHED_ERROR("RUN QUEUE IS FULL!");
            return;
        }
        
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->deadline > thread->deadline && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = RUNNABLE_QUEUE;
        queue->threads[pos] = thread;
        
    } else if (queue->type == PENDING_QUEUE)
    {
        if (queue->size == MAX_QUEUE)
        {
            RT_SCHED_ERROR("PENDING QUEUE IS FULL!");
            return;
        }
        
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->deadline > thread->deadline && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = PENDING_QUEUE;
        queue->threads[pos] = thread;
        
    } else if (queue->type == APERIODIC_QUEUE)
    {
        if (queue->size == MAX_QUEUE) {
            return;
        }
        
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->constraints->aperiodic.priority > thread->constraints->aperiodic.priority && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = APERIODIC_QUEUE;
        queue->threads[pos] = thread;
    } else if (queue->type == ARRIVAL_QUEUE) 
    {
        if (queue->size == MAX_QUEUE) {
            RT_SCHED_ERROR("ARRIVAL QUEUE IS FULL!");
            return;
        }

        queue->size++;
        uint64_t pos = queue->tail++;

        if (queue->tail == MAX_QUEUE) {
            queue->tail = 0;
        }

        thread->q_type = ARRIVAL_QUEUE;
		thread->status = ARRIVED;
        queue->threads[pos] = thread;
    } else if (queue->type == WAITING_QUEUE) 
    {
        if (queue->size == MAX_QUEUE) {
            RT_SCHED_ERROR("WAITING QUEUE IS FULL!");
            return;
        }

        queue->size++;
        uint64_t pos = queue->tail++;

        if (queue->tail == MAX_QUEUE) {
            queue->tail = 0;
        }
        thread->q_type = WAITING_QUEUE;
		thread->status = WAITING;
        queue->threads[pos] = thread;
    } else if (queue->type == SLEEPING_QUEUE) 
    {
        if (queue->size == MAX_QUEUE) {
            RT_SCHED_ERROR("WAITING QUEUE IS FULL!");
            return;
        }

        queue->size++;
        uint64_t pos = queue->tail++;

        if (queue->tail == MAX_QUEUE) {
            queue->tail = 0;
        }
        thread->q_type = SLEEPING_QUEUE;
        thread->status = SLEEPING;
        queue->threads[pos] = thread;
    } else if (queue->type == EXITED_QUEUE) 
    {
        if (queue->size == MAX_QUEUE) {
            RT_SCHED_ERROR("EXITED QUEUE IS FULL!");
            return;
        }

        queue->size++;
        uint64_t pos = queue->tail++;

        if (queue->tail == MAX_QUEUE) {
            queue->tail = 0;
        }
        thread->q_type = EXITED_QUEUE;
        queue->threads[pos] = thread;
    }

}

rt_thread* remove_thread(rt_thread *thread) {
    rt_queue *queue = NULL;
	queue_type type = thread->q_type;
    struct sys_info *sys = per_cpu_get(system);
    rt_scheduler *scheduler = sys->cpus[my_cpu_id()]->rt_sched;

	if (type == RUNNABLE_QUEUE) {
        queue = scheduler->runnable;

        if (queue == NULL) {
            RT_SCHED_ERROR("RUNNABLE QUEUE NOT FOUND\n");
            return NULL;
        }

		if (queue->size < 1) {
			RT_SCHED_ERROR("RUNNABLE QUEUE IS EMPTY. CAN'T REMOVE.\n");
            return NULL;
		}

        rt_thread *target_thread, *last;
        int i = 0, target_index = queue->size, now, child;
        for (i = 0; i < queue->size; i++) {
            if (thread = queue->threads[i]) {
                target_index = i;
                break;
            }
        }

        if (target_index == queue->size) {
            RT_SCHED_ERROR("THREAD NOT FOUND ON QUEUE\n");
            return NULL;
        }
        target_thread = queue->threads[target_index];
        last = queue->threads[--queue->size];

        for (now = target_index; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        return thread;
	} else if (type == PENDING_QUEUE) {
        queue = scheduler->pending;

        if (queue == NULL) {
            RT_SCHED_ERROR("PENDING QUEUE NOT FOUND\n");
            return NULL;
        }

        if (queue->size < 1) {
            RT_SCHED_ERROR("PENDING QUEUE IS EMPTY. CAN'T REMOVE.\n");
            return NULL;
        }

        rt_thread *target_thread, *last;
        int i = 0, target_index = queue->size, now, child;
        for (i = 0; i < queue->size; i++) {
            if (thread = queue->threads[i]) {
                target_index = i;
                break;
            }
        }

        if (target_index == queue->size) {
            RT_SCHED_ERROR("THREAD NOT FOUND ON QUEUE\n");
            return NULL;
        }
        target_thread = queue->threads[target_index];
        last = queue->threads[--queue->size];

        for (now = target_index; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        return thread;
    }  else if (type == APERIODIC_QUEUE) {
        queue = scheduler->aperiodic;

        if (queue == NULL) {
            RT_SCHED_ERROR("APERIODIC QUEUE NOT FOUND\n");
            return NULL;
        }

        if (queue->size < 1) {
            RT_SCHED_ERROR("APERIODIC QUEUE IS EMPTY. CAN'T REMOVE.\n");
            return NULL;
        }

        rt_thread *target_thread, *last;
        int i = 0, target_index = queue->size, now, child;
        for (i = 0; i < queue->size; i++) {
            if (thread = queue->threads[i]) {
                target_index = i;
                break;
            }
        }

        if (target_index == queue->size) {
            RT_SCHED_ERROR("THREAD NOT FOUND ON QUEUE\n");
            return NULL;
        }
        target_thread = queue->threads[target_index];
        last = queue->threads[--queue->size];

        for (now = target_index; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->constraints->aperiodic.priority < queue->threads[left_child(now)]->constraints->aperiodic.priority)
            {
                child = right_child(now);
            }
            
            if (last->constraints->aperiodic.priority > queue->threads[child]->constraints->aperiodic.priority)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        return thread;
    } else if (queue->type == ARRIVAL_QUEUE)
    {
        queue = scheduler->arrival;
        if (queue->head == queue->tail) {
            return NULL;
        }

        int i = queue->head;
        while (i != queue->tail) {
            if (queue->threads[i] == thread) {
                break;
            }
            i = (i + 1) % (MAX_QUEUE);
        }
        if (i == queue->tail) {
            RT_SCHED_ERROR("THREAD NOT FOUND.\n");
            return NULL;
        }

        while (i != queue->tail) {
            int next = ((i + 1) % (MAX_QUEUE));
            if (next == queue->tail) {
                break;
            }
            queue->threads[i] = queue->threads[next];
        }

        if (queue->tail == 0) {
            queue->tail = MAX_QUEUE - 1;
        } else {
            queue->tail--;
        }

        queue->size--;
        return thread;
    } else if (queue->type == WAITING_QUEUE)
    {
        queue = scheduler->waiting;

        if (queue->head == queue->tail) {
            return NULL;
        }

        int i = queue->head;
        while (i != queue->tail) {
            if (queue->threads[i] == thread) {
                break;
            }
            i = (i + 1) % (MAX_QUEUE);
        }
        if (i == queue->tail) {
            RT_SCHED_ERROR("THREAD NOT FOUND.\n");
            return NULL;
        }

        while (i != queue->tail) {
            int next = ((i + 1) % (MAX_QUEUE));
            if (next == queue->tail) {
                break;
            }
            queue->threads[i] = queue->threads[next];
        }
        if (queue->tail == 0) {
            queue->tail = MAX_QUEUE - 1;
        } else {
            queue->tail--;
        }
        queue->size--;
        return thread;
    }  else if (queue->type == SLEEPING_QUEUE)
    {
        queue = scheduler->sleeping;

        if (queue->head == queue->tail) {
            return NULL;
        }

        int i = queue->head;
        while (i != queue->tail) {
            if (queue->threads[i] == thread) {
                break;
            }
            i = (i + 1) % (MAX_QUEUE);
        }
        if (i == queue->tail) {
            RT_SCHED_ERROR("THREAD NOT FOUND.\n");
            return NULL;
        }

        while (i != queue->tail) {
            int next = ((i + 1) % (MAX_QUEUE));
            if (next == queue->tail) {
                break;
            }
            queue->threads[i] = queue->threads[next];
        }
        if (queue->tail == 0) {
            queue->tail = MAX_QUEUE - 1;
        } else {
            queue->tail--;
        }
        queue->size--;
        return thread;
    }

    return NULL;
}

rt_thread* dequeue_thread(rt_queue *queue)
{
    if (queue->type == RUNNABLE_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("RUNNABLE QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        
        rt_thread *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;

        if (min->status == TOBE_REMOVED) {
            min->status == REMOVED;
            return dequeue_thread(queue);
        }

        return min;
    } else if (queue->type == PENDING_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("PENDING QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        rt_thread *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;

        if (min->status == TOBE_REMOVED) {
            min->status == REMOVED;
            return dequeue_thread(queue);
        }

        return min;
    } else if (queue->type == APERIODIC_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("APERIODIC QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        rt_thread *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->constraints->aperiodic.priority < queue->threads[left_child(now)]->constraints->aperiodic.priority)
            {
                child = right_child(now);
            }
            
            if (last->constraints->aperiodic.priority > queue->threads[child]->constraints->aperiodic.priority)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;

        if (min->status == TOBE_REMOVED) {
            min->status == REMOVED;
            return dequeue_thread(queue);
        }

        return min;
    } else if (queue->type == ARRIVAL_QUEUE || queue->type = WAITING_QUEUE || queue->type == SLEEPING_QUEUE || queue->type == EXITED_QUEUE)
    {
        if (queue->head == queue->tail) {
            RT_SCHED_ERROR("QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        uint64_t pos = queue->head++;
        if (queue->head == MAX_QUEUE) {
            queue->head = 0;
        }
        queue->size--;

        rt_thread *t = queue->threads[pos];
        if (t->status == TOBE_REMOVED) {
            t->status == REMOVED;
            return dequeue_thread(queue);
        }

        return queue->threads[pos];
    }
    return NULL;
}

static void enqueue_thread_logic(rt_queue_sim *queue, rt_thread_sim *thread)
{
    if (queue->type == RUNNABLE_QUEUE)
    {
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->deadline > thread->deadline && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = RUNNABLE_QUEUE;
        queue->threads[pos] = thread;
        
    } else if (queue->type == PENDING_QUEUE)
    {
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->deadline > thread->deadline && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = PENDING_QUEUE;
        queue->threads[pos] = thread;
        
    } else if (queue->type == APERIODIC_QUEUE)
    {
        uint64_t pos = queue->size++;
        queue->threads[pos] = thread;
        while (queue->threads[parent(pos)]->constraints->aperiodic.priority > thread->constraints->aperiodic.priority && pos != parent(pos))
        {
            queue->threads[pos] = queue->threads[parent(pos)];
            pos = parent(pos);
        }
        thread->q_type = APERIODIC_QUEUE;
        queue->threads[pos] = thread;
    } 
}

static rt_thread_sim* dequeue_thread_logic(rt_queue_sim *queue)
{
    if (queue->type == RUNNABLE_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("RUNNABLE QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        
        rt_thread_sim *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        
        return min;
    } else if (queue->type == PENDING_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("PENDING QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }
        rt_thread_sim *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->deadline < queue->threads[left_child(now)]->deadline)
            {
                child = right_child(now);
            }
            
            if (last->deadline > queue->threads[child]->deadline)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        
        return min;
    } else if (queue->type == APERIODIC_QUEUE)
    {
        if (queue->size < 1)
        {
            RT_SCHED_ERROR("APERIODIC QUEUE EMPTY! CAN'T DEQUEUE!\n");
            return NULL;
        }

        rt_thread_sim *min, *last;
        int now, child;
        
        min = queue->threads[0];
        last = queue->threads[--queue->size];
        
        for (now = 0; left_child(now) < queue->size; now = child)
        {
            child = left_child(now);
            if (child < queue->size && queue->threads[right_child(now)]->constraints->aperiodic.priority < queue->threads[left_child(now)]->constraints->aperiodic.priority)
            {
                child = right_child(now);
            }
            
            if (last->constraints->aperiodic.priority > queue->threads[child]->constraints->aperiodic.priority)
            {
                queue->threads[now] = queue->threads[child];
            } else {
                break;
            }
        }
        
        queue->threads[now] = last;
        
        return min;
    } 
    return NULL;
}

void rt_thread_dump(rt_thread *thread)
{
    
    if (thread->type == PERIODIC)
    {
		printk("START TIME: %llu\t\tRUN TIME: %llu\t\tEXIT TIME: %llu\nDEADLINE: %llu\t\tCURRENT TIME: %llu\n", thread->start_time, thread->run_time, thread->exit_time, thread->deadline, cur_time() );
    } else if (thread->type == SPORADIC)
    {
        RT_SCHED_DEBUG("Work: %llu\t\t", thread->constraints->sporadic.work);
    }
}


static void set_timer(rt_scheduler *scheduler, rt_thread *current_thread, uint64_t end_time, uint64_t slack)
{
    scheduler->tsc->start_time = cur_time();
    struct sys_info *sys = per_cpu_get(system);
    struct apic_dev *apic = sys->cpus[my_cpu_id()]->apic;
    if (scheduler->pending->size > 0 && current_thread) {
        rt_thread *thread = scheduler->pending->threads[0];
        uint64_t completion_time = 0;
        if (current_thread->type == PERIODIC)
        {
            apic_oneshot_write(apic, umin(thread->deadline - end_time, (current_thread->constraints->periodic.slice - current_thread->run_time) + slack));
            scheduler->tsc->set_time = umin(thread->deadline - end_time, (current_thread->constraints->periodic.slice - current_thread->run_time));
        } else if (current_thread->type == SPORADIC)
        {
            apic_oneshot_write(apic, umin(thread->deadline - end_time, (current_thread->constraints->sporadic.work - current_thread->run_time) + slack));
            scheduler->tsc->set_time = umin(thread->deadline - end_time, (current_thread->constraints->sporadic.work - current_thread->run_time) + slack);
        } else
        {
            apic_oneshot_write(apic, umin(thread->deadline - end_time, QUANTUM));
            scheduler->tsc->set_time = umin(thread->deadline - end_time, QUANTUM);
        }
    } else if (scheduler->pending->size == 0 && current_thread) {
        if (current_thread->type == PERIODIC)
        {
            apic_oneshot_write(apic, (current_thread->constraints->periodic.slice - current_thread->run_time) + slack);
            scheduler->tsc->set_time = (current_thread->constraints->periodic.slice - current_thread->run_time) + slack;
        } else if (current_thread->type == SPORADIC)
        {
            apic_oneshot_write(apic, (current_thread->constraints->sporadic.work - current_thread->run_time) + slack);
            scheduler->tsc->set_time = (current_thread->constraints->sporadic.work - current_thread->run_time) + slack;
        }
        else {
            apic_oneshot_write(apic, QUANTUM);
            scheduler->tsc->set_time = QUANTUM;
        }
    } else {
        apic_oneshot_write(apic, QUANTUM);
        scheduler->tsc->set_time = QUANTUM;
    }
	scheduler->tsc->end_time = end_time;
}


struct nk_thread *rt_need_resched()
{
    struct sys_info *sys = per_cpu_get(system);
    rt_scheduler *scheduler = sys->cpus[my_cpu_id()]->rt_sched;
    
    struct nk_thread *c = get_cur_thread();
    rt_thread *rt_c = c->rt_thread;
    
    
    uint64_t end_time = scheduler->run_time + cur_time();
    uint64_t slack = 0;
    scheduler->tsc->end_time = cur_time();
    
    rt_thread *rt_n = NULL;
    
    while (scheduler->pending->size > 0)
    {
        if (scheduler->pending->threads[0]->deadline < end_time)
        {
            rt_thread *arrived_thread = dequeue_thread(scheduler->pending);

            if (arrived_thread == NULL) {
                continue;
            }

            update_periodic(arrived_thread);
            enqueue_thread(scheduler->runnable, arrived_thread);
            continue;
        } else
        {
            break;
        }
    }
    
    switch (rt_c->type) {
        case APERIODIC:
            rt_c->constraints->aperiodic.priority = rt_c->run_time;
            enqueue_thread(scheduler->aperiodic, rt_c);

            if (scheduler->runnable->size > 0)
            {
                rt_n = dequeue_thread(scheduler->runnable);
                if (rt_n != NULL) {
                    set_timer(scheduler, rt_n, end_time, slack);
                    return rt_n->thread;
                }
            }

            rt_n = dequeue_thread(scheduler->aperiodic);
            if (rt_n == NULL) {
                    RT_SCHED_ERROR("APERIODIC QUEUE IS EMPTY.\n THE WORLD IS GOVERNED BY MADNESS.\n");
                    panic("ATTEMPTING TO RUN A NULL RT_THREAD.\n");
            }
            set_timer(scheduler, rt_n, end_time, slack);
            return rt_n->thread;
            break;
            
        case SPORADIC:
            if (rt_c->run_time >= rt_c->constraints->sporadic.work) {
                check_deadlines(rt_c);

                if (scheduler->runnable->size > 0) {
                    rt_n = dequeue_thread(scheduler->runnable);
                    if (rt_n != NULL) {
                        set_timer(scheduler, rt_n, end_time, slack);
                        return rt_n->thread;
                    }
                }
                rt_n = dequeue_thread(scheduler->aperiodic);
                if (rt_n == NULL) {
                    RT_SCHED_ERROR("APERIODIC QUEUE IS EMPTY.\n THE WORLD IS GOVERNED BY MADNESS.\n");
                    panic("ATTEMPTING TO RUN A NULL RT_THREAD.\n");
                }
                set_timer(scheduler, rt_n, end_time, slack);
                return rt_n->thread;
            } else {
                if (scheduler->runnable->size > 0)
                {
                    if (rt_c->deadline > scheduler->runnable->threads[0]->deadline) {
                        rt_n = dequeue_thread(scheduler->runnable);
                        if (rt_n != NULL) {
                            enqueue_thread(scheduler->runnable, rt_c);
                            set_timer(scheduler, rt_n, end_time, slack);
                            return rt_n->thread;
                        }
                    }
                }
            }
            set_timer(scheduler, rt_c, end_time, slack);
            return rt_c->thread;
            break;
            
        case PERIODIC:
            if (rt_c->run_time >= rt_c->constraints->periodic.slice) {
                if (check_deadlines(rt_c)) {
                    update_periodic(rt_c);
                    enqueue_thread(scheduler->runnable, rt_c);
                } else {
                    enqueue_thread(scheduler->pending, rt_c);
                }

                if (scheduler->runnable->size > 0) {
                    rt_n = dequeue_thread(scheduler->runnable);
                    if (rt_n != NULL) {
                        set_timer(scheduler, rt_n, end_time, slack);
                        return rt_n->thread;
                    }
                }
                rt_n = dequeue_thread(scheduler->aperiodic);
                if (rt_n == NULL) {
                    RT_SCHED_ERROR("APERIODIC QUEUE IS EMPTY.\n THE WORLD IS GOVERNED BY MADNESS.\n");
                    panic("ATTEMPTING TO RUN A NULL RT_THREAD.\n");
                }
                set_timer(scheduler, rt_n, end_time, slack);
                return rt_n->thread;
            } else {
                if (scheduler->runnable->size > 0)
                {
                    if (rt_c->deadline > scheduler->runnable->threads[0]->deadline) {
                        rt_n = dequeue_thread(scheduler->runnable);
                        if (rt_n != NULL) {
                            enqueue_thread(scheduler->runnable, rt_c);
                            set_timer(scheduler, rt_n, end_time, slack);
                            return rt_n->thread;
                        }
                    }
                }
            }
            set_timer(scheduler, rt_c, end_time, slack);
            return rt_c->thread;
            break;
        default:
            set_timer(scheduler, rt_c, end_time, slack);
            return c;
    }
}



static int check_deadlines(rt_thread *t)
{
    if (t->exit_time > t->deadline) {
        RT_SCHED_ERROR("Missed Deadline = %llu\t\t Current Timer = %llu\n", t->deadline, t->exit_time);
        RT_SCHED_ERROR("Difference =  %llu\n", t->exit_time - t->deadline);
		
		rt_thread_dump(t);
		return 1;
    }
    return 0;
}

static inline void update_periodic(rt_thread *t)
{
    if (t->type == PERIODIC)
    {
        t->deadline  = cur_time() + t->constraints->periodic.period;
        t->run_time = 0;
    }
}

uint64_t cur_time()
{
    return rdtsc();
}


int rt_admit(rt_scheduler *scheduler, rt_thread *thread)
{
    if (thread->type == PERIODIC)
    {
        uint64_t per_util = get_per_util(scheduler->runnable, scheduler->pending);
        printk("UTIL FACTOR =  \t%llu\n", per_util);
        
        if ((per_util + (thread->constraints->periodic.slice * 100000) / thread->constraints->periodic.period) > PERIODIC_UTIL) {
            RT_SCHED_ERROR("PERIODIC: Admission denied utilization factor overflow!\n");
            return 0;
        }
    } else if (thread->type == SPORADIC)
    {
        uint64_t spor_util = get_spor_util(scheduler->runnable);
        
        if (spor_util > SPORADIC_UTIL) {
            RT_SCHED_DEBUG("SPORADIC: Admission denied utilization factor overflow!\n");
            return 0;
        }
    }

    return 1;
}

static inline uint64_t get_avg_per(rt_queue *runnable, rt_queue *pending, rt_thread *new_thread)
{
    uint64_t sum_period = 0;
    uint64_t num_periodic = 0;
    int i;
    
    for (i = 0; i < runnable->size; i++)
    {
        rt_thread *thread = runnable->threads[i];
        if (thread->type == PERIODIC) {
            sum_period += thread->constraints->periodic.period;
            num_periodic++;
        }
    }
    
    for (i = 0; i < pending->size; i++)
    {
        rt_thread *thread = pending->threads[i];
        if (thread->type == PERIODIC) {
            sum_period += thread->constraints->periodic.period;
            num_periodic++;
        }
    }
    
    if (new_thread->type == PERIODIC)
    {
        sum_period += new_thread->constraints->periodic.period;
        num_periodic++;
    }
    
    sum_period += QUANTUM;
    num_periodic++;
    return (sum_period / num_periodic);
}

static inline uint64_t get_min_per(rt_queue *runnable, rt_queue *pending, rt_thread *thread)
{
    uint64_t min_period = 0xFFFFFFFFFFFFFFFF;
    int i;
    for (i = 0; i < runnable->size; i++)
    {
        rt_thread *thread = runnable->threads[i];
        if (thread->type == PERIODIC)
        {
            min_period = MIN(thread->constraints->periodic.period, min_period);
        }
    }
    
    for (i = 0; i < pending->size; i++)
    {
        rt_thread *thread = pending->threads[i];
        if (thread->type == PERIODIC)
        {
            min_period = MIN(thread->constraints->periodic.period, min_period);
        }
    }
    return min_period;
}

static inline uint64_t get_per_util(rt_queue *runnable, rt_queue *pending)
{
    uint64_t util = 0;
    
    int i;
    for (i = 0; i < runnable->size; i++)
    {
        rt_thread *thread = runnable->threads[i];
        if (thread->type == PERIODIC) {
            util += (thread->constraints->periodic.slice * 100000) / thread->constraints->periodic.period;
        }
    }
    
    for (i = 0; i < pending->size; i++)
    {
        rt_thread *thread = pending->threads[i];
        if (thread->type == PERIODIC) {
            util += (thread->constraints->periodic.slice * 100000) / thread->constraints->periodic.period;
        }
    }
    
    return util;
}

static inline uint64_t get_spor_util(rt_queue *runnable)
{
    uint64_t util = 0;
    
    int i;
    for (i = 0; i < runnable->size; i++)
    {
        rt_thread *thread = runnable->threads[i];
        if (thread->type == SPORADIC)
        {
            util += (thread->constraints->sporadic.work * 100000) / (thread->deadline - cur_time());
        }
    }
    return util;
}

static void test_real_time(void *in)
{
    while (1)
    {
		printk("Inside thread %d\n", *(int *)in);
        udelay(10000000);
    }
}

void rt_start(uint64_t sched_slice_time, uint64_t sched_period) {
    nk_thread_id_t sched;
    rt_constraints *constraints_first = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_first = {sched_period, sched_slice_time};
    constraints_first->periodic = per_constr_first;

    nk_thread_start_sim((nk_thread_fun_t)sched_sim, NULL, NULL, 0, 0, &sched, my_cpu_id(), PERIODIC, constraints_first, 0);
}

static void sched_sim(void *scheduler) {
	nk_thread_id_t v;	
	int five = 5;
	rt_constraints *constraints_fifth = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_fifth = {(5000000), (500000)};
    constraints_fifth->periodic = per_constr_fifth;
	
    nk_thread_start_sim((nk_thread_fun_t)test_real_time, (void *)five, NULL, 0, 0, &v, my_cpu_id(), PERIODIC, constraints_fifth, 0);
	while (1) {
		printk("Running the scheduler on core %d\n", my_cpu_id());
		udelay(100000);
	}
	rt_simulator *sim = init_simulator();

    struct sys_info *sys = per_cpu_get(system);
    rt_scheduler *sched = sys->cpus[my_cpu_id()]->rt_sched;

    while (1) {
        // Admit the new queues
        rt_thread *new = dequeue_thread(sched->arrival);
        if (new != NULL) {
            int admission_check = rt_admit(sched, new);
            if (admission_check) {
                copy_threads_sim(sim, sched);

                free_threads_sim(sim);
            }
        }

        // Remove the exited queues
        rt_thread *d = dequeue_thread(sched->exited);
        while (d != NULL) {
            rt_thread *e = remove_thread(d); 
            if (d->status != REMOVED && e == NULL) {
                RT_SCHED_ERROR("REMOVING THREAD INCORRECTLY.\n");
            } else {
                d->status == REMOVED;
            }
        }

        // Cleanup the deleted queues
        
    }
}

static rt_thread* max_periodic(rt_scheduler *scheduler) {
    rt_queue *runnable = scheduler->runnable;
    rt_queue *pending = scheduler->pending;
    rt_thread *max_thread = NULL;  
    uint64_t max_period = 0;
    int i = 0;
    for (i = 0; i < runnable->size; i++) {
        rt_thread *thread = runnable->threads[i];
        if (thread->type == PERIODIC) {
            if (thread->constraints->periodic.period > max_period) {
                max_period = thread->constraints->periodic.period;
                max_thread = thread;
            }
        }
    }
    
    for (i = 0; i < pending->size; i++) {
        rt_thread *thread = pending->threads[i];
        if (thread->type == PERIODIC) {
            if (thread->constraints->periodic.period > max_period) {
                max_period = thread->constraints->periodic.period;
                max_thread = thread;
            }
        }
    }
    return max_thread;
}

static void copy_threads_sim(rt_simulator *simulator, rt_scheduler *scheduler) {
    int i;
    simulator->runnable->size = scheduler->runnable->size;
    for (i = 0; i < simulator->runnable->size; i++) {
        rt_thread *s = scheduler->runnable->threads[i];
        rt_thread_sim *d = (rt_thread_sim *)malloc(sizeof(rt_thread_sim));
        d->type = s->type;
        d->q_type = s->q_type;
        d->status = s->status;

        rt_constraints *constraints = (rt_constraints *)malloc(sizeof(rt_constraints));
        if (d->type == PERIODIC) {
            struct periodic_constraints constr = {(s->constraints->periodic.period), (s->constraints->periodic.slice)};
            constraints->periodic = constr;
        } else if (d->type == SPORADIC) {
            struct sporadic_constraints constr = {(s->constraints->sporadic.work)};
            constraints->sporadic = constr;
        }
        d->constraints = constraints;
        d->start_time = s->start_time;
        d->run_time = s->run_time;
        d->deadline = s->deadline;
        d->exit_time = s->exit_time;
        simulator->runnable->threads[i] = d;
    }

    simulator->aperiodic->size = scheduler->aperiodic->size;
    for (i = 0; i < simulator->aperiodic->size; i++) {
        rt_thread *s = scheduler->aperiodic->threads[i];
        rt_thread_sim *d = (rt_thread_sim *)malloc(sizeof(rt_thread_sim));
        d->type = s->type;
        d->q_type = s->q_type;
        d->status = s->status;

        rt_constraints *constraints = (rt_constraints *)malloc(sizeof(rt_constraints));
        struct aperiodic_constraints constr = {(s->constraints->aperiodic.priority)};
        constraints->aperiodic = constr;

        d->constraints = constraints;
        d->start_time = s->start_time;
        d->run_time = s->run_time;
        d->deadline = s->deadline;
        d->exit_time = s->exit_time;
        simulator->aperiodic->threads[i] = d;
    }

    simulator->pending->size = scheduler->pending->size;
    for (i = 0; i < simulator->pending->size; i++) {
        rt_thread *s = scheduler->pending->threads[i];
        rt_thread_sim *d = (rt_thread_sim *)malloc(sizeof(rt_thread_sim));
        d->type = s->type;
        d->q_type = s->q_type;
        d->status = s->status;

        rt_constraints *constraints = (rt_constraints *)malloc(sizeof(rt_constraints));
        if (d->type == PERIODIC) {
            struct periodic_constraints constr = {(s->constraints->periodic.period), (s->constraints->periodic.slice)};
            constraints->periodic = constr;
        } else if (d->type == SPORADIC) {
            struct sporadic_constraints constr = {(s->constraints->sporadic.work)};
            constraints->sporadic = constr;
        }

        d->constraints = constraints;
        d->start_time = s->start_time;
        d->run_time = s->run_time;
        d->deadline = s->deadline;
        d->exit_time = s->exit_time;
        simulator->pending->threads[i] = d;
    }
}

static void free_threads_sim(rt_simulator *simulator) {
    int i = 0;
    for (i = 0; i < simulator->runnable->size; i++) {
        free(simulator->runnable->threads[i]->constraints);
        free(simulator->runnable->threads[i]);
    }
    simulator->runnable->size = 0;

    for (i = 0; i < simulator->aperiodic->size; i++) {
        free(simulator->aperiodic->threads[i]->constraints);
        free(simulator->aperiodic->threads[i]);
    }
    simulator->aperiodic->size = 0;

    for (i = 0; i < simulator->pending->size; i++) {
        free(simulator->pending->threads[i]->constraints);
        free(simulator->pending->threads[i]);
    }
    simulator->pending->size = 0;
}

static rt_thread_sim* rt_need_resched_logic(rt_simulator *simulator, rt_thread_sim *thread, uint64_t time)
{
    rt_thread_sim *next;
    while (simulator->pending->size > 0)
    {
        if (simulator->pending->threads[0]->deadline < time)
        {
            rt_thread_sim *arrived_thread = dequeue_thread_logic(simulator->pending);
            enqueue_thread_logic(simulator->runnable, arrived_thread);
            continue;
        } else
        {
            break;
        }
    }
    
    switch (thread->type) {
        case APERIODIC:
            thread->constraints->aperiodic.priority = thread->run_time;
            
            if (simulator->runnable->size > 0)
            {
                enqueue_thread_logic(simulator->aperiodic, thread);
                next = dequeue_thread_logic(simulator->runnable);
                set_timer_logic(simulator, next, time);
                return next;
            }

            enqueue_thread_logic(simulator->aperiodic, thread);
            next = dequeue_thread_logic(simulator->aperiodic);
            set_timer_logic(simulator, next, time);
            return next;

            break;
            
        case SPORADIC:
            if (thread->run_time >= thread->constraints->sporadic.work) {
                if (simulator->runnable->size > 0) {
                    next = dequeue_thread_logic(simulator->runnable);
                    set_timer_logic(simulator, next, time);
                    return next;
                }
                next = dequeue_thread_logic(simulator->aperiodic);
                set_timer_logic(simulator, next, time);
                return next;
            } else {
                if (simulator->runnable->size > 0)
                {
                    if (thread->deadline > simulator->runnable->threads[0]->deadline) {
                        next = dequeue_thread_logic(simulator->runnable);
                        enqueue_thread_logic(simulator->runnable, thread);
                        set_timer_logic(simulator, next, time);
                        return next;
                    }
                }
            }

            set_timer_logic(simulator, thread, time);
            return thread;
            break;
            
        case PERIODIC:
            if (thread->run_time >= thread->constraints->periodic.slice) {
                if (check_deadlines_logic(thread, time)) {
                    update_periodic_logic(thread, time);
                    enqueue_thread_logic(simulator->runnable, thread);
                } else {
                    enqueue_thread_logic(simulator->pending, thread);
                }
                if (simulator->runnable->size > 0) {
                    next = dequeue_thread_logic(simulator->runnable);
                    set_timer_logic(simulator, next, time);
                    return next;
                }
                next = dequeue_thread_logic(simulator->aperiodic);
                set_timer_logic(simulator, next, time);
                return next;
            } else {
                if (simulator->runnable->size > 0)
                {
                    if (thread->deadline > simulator->runnable->threads[0]->deadline) {
                        next = dequeue_thread_logic(simulator->runnable);
                        enqueue_thread_logic(simulator->runnable, thread);
                        set_timer_logic(simulator, next, time);
                        return next;
                    }
                }
            }
            set_timer_logic(simulator, thread, time);
            return thread;
        default:
            set_timer_logic(simulator, thread, time);
            return thread;
    }
    
}

static inline void update_exit_logic(rt_thread_sim *t, uint64_t time)
{
    t->run_time += (t->exit_time - t->start_time);
}

static inline void update_enter_logic(rt_thread_sim *t, uint64_t time)
{
    t->start_time = time;
}

static int check_deadlines_logic(rt_thread_sim *t, uint64_t time)
{
    if (time > t->deadline) {
        return 1;
    }
    return 0;
}

static inline void update_periodic_logic(rt_thread_sim *t, uint64_t time)
{
    if (t->type == PERIODIC)
    {
        t->deadline  = time + t->constraints->periodic.period;
        t->run_time = 0;
    }
}

static uint64_t set_timer_logic(rt_simulator *simulator, rt_thread_sim *thread, uint64_t time)
{
    if (simulator->pending->size > 0 && thread) {
        rt_thread_sim *next = simulator->pending->threads[0];
        if (thread->type == PERIODIC)
        {
            return umin(next->deadline - time, (thread->constraints->periodic.slice - thread->run_time));
        } else
        {
            return umin(next->deadline - time, QUANTUM);
        }
    } else if (simulator->pending->size == 0 && thread) {
        if (thread->type == PERIODIC)
        {
            return (thread->constraints->periodic.slice - thread->run_time);
        } else {
            return QUANTUM;
        }
    } else {
        return QUANTUM;
    }
}



void nk_rt_test()
{
    nk_thread_id_t r;
    nk_thread_id_t s;
    nk_thread_id_t t;
    nk_thread_id_t u;
    nk_thread_id_t v;
    nk_thread_id_t w;
    nk_thread_id_t x;
    nk_thread_id_t y;
    
    
    
    rt_constraints *constraints_first = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_first = {(10000000000), (10000000)};
    constraints_first->periodic = per_constr_first;
    
    rt_constraints *constraints_second = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_second = {(5000000000), (5000000)};
    constraints_second->periodic = per_constr_second;
    
    rt_constraints *constraints_third = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_third = {(250000000), (250000)};
    constraints_third->periodic = per_constr_third;
    
    rt_constraints *constraints_fifth = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_fifth = {(5000000), (500000)};
    constraints_fifth->periodic = per_constr_fifth;
    
    rt_constraints *constraints_six = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_six = {(5000000000), (5000000)};
    constraints_six->periodic = per_constr_six;
    
    rt_constraints *constraints_seven = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_seven = {(5000000000), (5000000)};
    constraints_seven->periodic = per_constr_seven;
    
    rt_constraints *constraints_fourth = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct aperiodic_constraints aper_constr = {2};
    constraints_fourth->aperiodic = aper_constr;
    
    rt_constraints *constraints_eighth = (rt_constraints *)malloc(sizeof(rt_constraints));
    struct periodic_constraints per_constr_eighth = {(500000000000), (500000000)};
    constraints_eighth->periodic = per_constr_eighth;

    uint64_t first = 1, second = 2, third = 3, fourth = 4, five = 5, six = 6, seven = 7, eight = 8;
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)first, NULL, 0, 0, &r, my_cpu_id(), PERIODIC, constraints_first, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)second, NULL, 0, 0, &s, my_cpu_id(), PERIODIC, constraints_second, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)third, NULL, 0, 0, &t, my_cpu_id(), PERIODIC, constraints_third, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)five, NULL, 0, 0, &v, my_cpu_id(), PERIODIC, constraints_fifth, 0);
	nk_thread_start((nk_thread_fun_t)test_real_time, (void *)six, NULL, 0, 0, &w, my_cpu_id(), PERIODIC, constraints_six, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)seven, NULL, 0, 0, &x, my_cpu_id(), PERIODIC, constraints_seven, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)fourth, NULL, 0, 0, &u, my_cpu_id(), APERIODIC, constraints_fourth, 0);
    nk_thread_start((nk_thread_fun_t)test_real_time, (void *)eight, NULL, 0, 0, &y, my_cpu_id(), PERIODIC, constraints_eighth, 0);	
}

static inline uint64_t umin(uint64_t x, uint64_t y)
{
    return (x < y) ? x : y;
}

void rt_thread_exit(rt_thread *thread) {
    thread->status = TOBE_REMOVED;
    struct sys_info *sys = per_cpu_get(system);
    rt_scheduler *sched = sys->cpus[my_cpu_id()]->rt_sched;
    enqueue_thread(sched->exited, thread);
}

void rt_thread_free(rt_thread *thread) {

}






