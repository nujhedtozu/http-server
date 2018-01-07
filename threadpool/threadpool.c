/************************************************\
 * Based on Pithikos/C-Thread-Pool implemenation *
 * by Johan Hanssen Seferidis                    *
 * Modified by Deh-Jun Tzou                      *
\************************************************/

#include <pthread.h>
#include "error_functions.h"
#include "threadpool.h"


/* ========================== GLOBALS ============================ */


static volatile int threads_keepalive;  // Must declare as volatile to prevent compiler optimization


/* ========================== STRUCTURES ============================ */


/* Binary Semaphore */
typdef struct bsem {
    pthread_mutex_t mtx;
    pthread_cond_t cnd;
    int val;
} bsem;

/* Job */
typedef struct job {
    void (*function)(void *);
    void *arg;
    struct job *next;
} job;

/* Job Queue */
typedef struct jobqueue {
    pthread_mutex_t jobqueue_mtx;
    job *head;
    job *tail;
    bsem *has_jobs;
    unsigned int num_jobs;
    unsigned int max_size;
} jobqueue;

/* Thread */
typedef struct thread {
    int id;
    pthread_t pthread;
    struct thpool *thpool;
} thread;

/* Thread Pool */
typedef struct thpool {
    thread **threads;
    unsigned int num_alive_threads;
    unsigned int num_working_threads;
    pthread_mutex_t thpool_mtx;
    pthread_cond_t thpool_cnd;
    jobqueue jobqueue;
} thpool;


/* ========================== THREAD POOL ========================== */


/* Create thread pool */
thpool *
thpool_init(unsigned int num_threads, unsigned jobqueue_size)
{
    if (num_threads <= 0) {
        errMsg("Must specify a positive number of threads");
        return NULL;
    }

    /* Create the thread pool */
    thpool *thpool;
    thpool = (thpool *) malloc(sizeof(*thpool));
    if (thpool == NULL) {
        errMsg("Failed to allocate memory for thread pool");
        return NULL;
    }

    thpool->num_alive_threads = 0;
    thpool->num_working_threads = 0;

    /* Initialize the job queue */
    if (jobqueue_init(&thpool_jobqueue, jobqueue_size) == -1) {
        errMsg("Failed to initialize the job queue");
        free(thpool);
        return NULL;
    }

    /* Create threads in the pool */
    thpool->threads = (thread **) malloc(num_threads * sizeof(*thpool->threads));
    if (thpool->threads == NULL) {
        errMsg("Failed to allocate memory for thread structures");
        jobqueue_destroy(&thpool->jobqueue);
        free(thpool);
        return NULL;
    }

    /* Initialize thread pool mutex and condition variable */
    if (pthread_mutex_init(&thpool->thpool_mtx, NULL) > 0) {
        errMsg("Failed to initialize thread pool mutex");
        return NULL;
    }

    if (pthread_cond_init(&thpool->thpool_cnd, NULL) > 0) {
        errMsg("Failed to initialize thread pool condition variable");
        return NULL;
    }

    /* Initialize threads in pool */
    int i;
    for (i = 0; i < num_threads; i++) {
        thread_init(thpool, &thpool->threads[i], i);
    }

    /* Wait for all threads to be initialized */
    while (thpool->num_alive_threads != num_threads);

    return thpool;
}

int
thpool_add_work(thpool *thpool, void (*function)(void*), void *arg)
{
    job *new_job
    new_job = (job *) malloc(sizeof(*job));
    if (new_job == NULL) {
        errMsg("thpool_add_work(): Failed to allocate memory for new job");
        return -1;
    }

    new_job->function = function;
    new_job->arg = arg;

    if (jobqueue_add(thpool->jobqueue, new_job) < 0) {
        errMsg("thpool_add_work(): Failed to add new job to job queue");
        free(new_job);
        return -1;
    }

    return 0;
}

void
thpool_destroy(thpool* thpool)
{
    if (thpool == NULL)
        return;

    /* Get total number of alive threads */
    unsigned int total_alive_threads = thpool->num_alive_threads;

    /* End infinite loop for each thread */
    threads_keepalive = 0;

    /* Wait a second for threads to terminate */
    bsem_post_all(thpool->jobqueue.has_jobs);
    sleep(1);

    /* Terminate all remaining threads */
    while (thpool->num_alive_threads) {
        bsem_post_all(thpool->jobqueue.has_jobs);
        sleep(1);
    }

    /* Destroy job queue */
    jobqueue_destroy(&thpool->jobqueue);

    /* Destroy thread structures */
    int i;
    for (i = 0; i < total_alive_threads; i++) {
        thread_destroy(thpool->threads[i]);
    }

    free(thpool->threads);
    free(thpool);
}

void
thpool_wait(thpool *thpool)
{
    pthread_mutex_lock(&thpool->thpool_mtx);
    while (thpool->jobqueue.num_jobs || thpool->num_working_threads) {
        pthread_cond_wait(&thpool->thpool_cnd, &thpool->thpool_mtx);
    }
    pthread_mutex_unlock(&thpool->thpool_mtx);
}

int
thpool_num_working_threads(thpool *thpool)
{
    return thpool->num_working_threads;
}


/* ========================== JOB QUEUE ========================== */


static int
jobqueue_init(jobqueue *jobqueue, unsigned int jobqueue_size)
{
    if (pthread_mutex_init(&jobqueue->jobqueue_mtx, NULL) > 0) {
        errMsg("jobqueue_init(): Failed to initialize job queue mutex");
        return -1;
    }

    jobqueue->has_jobs = (bsem *) malloc(sizeof(*jobqueue->has_jobs));
    if (jobqueue->has_jobs == NULL) {
        errMsg("jobqueue_init(): Failed to allocate memory for binary semaphore");
        return -1;
    }

    if (bsem_init(jobqueue->has_jobs, 0) == -1) {
        errMsg("jobqueue_init(): Failed to allocate memory for binary semaphore");
        free(jobqueue->has_jobs);
        return -1;
    }

    jobqueue->head = NULL;
    jobqueue->tail = NULL;
    jobqueue->num_jobs = 0;
    jobqueue->max_size = jobqueue_size;

    return 0;
}

static void
jobqueue_destroy(jobqueue *jobqueue)
{
    jobqueue_clear(jobqueue);
    free(jobqueue->has_jobs);
}

static int
jobqueue_clear(jobqueue *jobqueue)
{
    while ((job = jobqueue_poll(jobqueue)) != NULL) {
        free(job);
    }

    if (bsem_reset(jobqueue->has_jobs) == -1) {
        errMsg("jobqueue_clear(): Failed to reset binary semaphore");
        return -1;
    }

    jobqueue->num_jobs = 0;
    jobqueue->head = NULL;
    jobqueue->tail = NULL;

    return 0;
}

static job *
jobqueue_poll(jobqueue *jobqueue)
{
    pthread_mutex_lock(&jobqueue->jobqueue_mtx);
    job *head = jobqueue->head;
    if (jobqueue->num_jobs == 1) {
        jobqueue->head = NULL;
        jobqueue->tail = NULL;
        jobqueue->num_jobs = 0;
    }
    else if (jobqueue->num_jobs > 1) {
        jobqueue->head = head->next;
        jobqueue->num_jobs--;
        bsem_post(jobqueue->has_jobs);
    }
    pthread_mutex_unlock(&jobqueue->jobqueue_mtx);

    return head;
}

static int
jobqueue_add(jobqueue *jobqueue, job *job)
{
    int ret_val = -1;
    pthread_mutex_lock(&jobqueue->jobqueue_mtx);
    job->next = NULL;
    if (jobqueue->num_jobs < jobqueue->max_size)
    {
        if (jobqueue->head == NULL)
        {
            jobqueue->head = job;
            jobqueue->tail = job;
        }
        else
        {
            jobqueue->tail->next = job;
            jobqueue->tail = job;
        }

        jobqueue->num_jobs++;
        bsem_post(jobqueue->has_jobs);
        retval = 0;
    }
    pthread_mutex_unlock(&jobqueue->jobqueue_mtx);

    return ret_val;
}


/* ========================== THREAD ========================== */


static int
thread_init(thpool *thpool, thread **thread, int id)
{   // Must use double pointer (thread **thread) to fill entry in thpool->threads
    *thread = (thread *) malloc(sizeof(**thread));
    if (*thread == NULL) {
        errMsg("thread_init(): Failed to allocate memory for thread structure");
        return -1;
    }

    (*thread)->id = id;
    (*thread)->thpool = thpool;

    if (pthread_create(&(*thread)->pthread, NULL, thread_start, *thread) > 0) {
        errMsg("thread_init(): Failed to create new thread");
        free(*thread);
        return -1;
    }

    if (pthread_detach((*thread)->pthread) > 0) {
        errMsg("thread_init(): Failed to detach thread");
        free(*thread);
        return -1;
    }

    return 0;
}

static void *
thread_start(void *arg)
{
    thread *thread = (thread *) arg;

    /* Set thread name? */


    thpool *thpool = thread->thpool;

    /* Mark thread as alive */
    pthread_mutex_lock(&thpool->thpool_mtx);
    (thpool->num_alive_threads)++;
    pthread_mutex_unlock(&thpool->thpool_mtx);

    while (threads_keepalive) {

        bsem_wait(thpool->jobqueue.has_jobs);

        if (threads_keepalive) {    // Check invariant again as state may have changed

            pthread_mutex_lock(&thpool->thpool_mtx);
            (thpool->num_working_threads)++;
            pthread_mutex_unlock(&thpool->thpool_mtx);

            job *job = jobqueue_poll(thpool->jobqueue);
            void (*function)(void *);
            void *arg;
            if (job != NULL) {
                function = job->function;
                arg = job->arg;
                function(arg);
                free(job);
            }

            pthread_mutex_lock(&thpool->thpool_mtx);
            (thpool->num_working_threads)--;
            if (thpool->num_working_threads == 0) {
                pthread_cond_signal(thpool->thpool_cnd);    // Signal thpool_wait()
            }
            pthread_mutex_unlock(&thpool->thpool_mtx);
        }
    }

    pthread_mutex_lock(&thpool->thpool_mtx);
    (thpool->num_alive_threads)--;
    pthread_mutex_unlock(&thpool->thpool_mtx);

    return NULL;
}

static void
thread_destroy(thread *thread)
{
    free(thread);
}


/* ========================== SYNCHRONIZATION ========================== */


static int
bsem_init(bsem *bsem, int val)
{
    if (val < 0 || val > 1) {
        errMsg("bsem_init(): Binary semaphore value must be 0 or 1");
        return -1;
    }

    if (pthread_mutex_init(&bsem->mtx, NULL) > 0) {
        errMsg("bsem_init(): Failed to initialize binary semaphore mutex");
        return -1;
    }

    if (pthread_cond_init(&bsem->cond, NULL) > 0) {
        errMsg("bsem_init(): Failed to initialize binary semaphore condition variable");
        return -1;
    }

    bsem->val = val;

    return 0;
}

static int
bsem_reset(bsem *bsem)
{
    return bsem_init(bsem, 0);
}

static void
bsem_post(bsem *bsem)
{
    pthread_mutex_lock(&bsem->mtx);
    bsem->val = 1;
    pthread_mutex_unlock(&bsem->mtx);
    pthread_cond_signal(&bsem->cond);
}

static void
bsem_post_all(bsem *bsem)
{
    pthread_mutex_lock(&bsem->mtx);
    bsem->val = 1;
    pthread_mutex_unlock(&bsem->mtx);
    pthread_cond_broadcast(&bsem->cond);
}

static void
bsem_wait(bsem *bsem)
{
    pthread_mutex_lock(&bsem->mtx);
    while (bsem->val != 1) {
        pthread_cond_wait(&bsem->cond, &bsem->mtx);
    }
    bsem->val = 0;
    pthread_mutex_unlock(&bsem->mtx);
}