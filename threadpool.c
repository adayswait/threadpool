/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <limits.h>
#include <errno.h>
#include <sys/resource.h>

#include "queue.h"
#include "threadpool.h"

#define MAX_THREADPOOL_SIZE 128

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_cond_t cond;
static pthread_mutex_t mutex;
static unsigned int idle_threads;
static unsigned int nthreads;
static pthread_t *threads;
static pthread_t default_threads[4];
static QUEUE exit_message;
static QUEUE wq;

static void tp_cancelled(struct tp_work *w)
{
  abort();
}

void tp_sem_post(sem_t *sem)
{
  if (sem_post(sem))
    abort();
}

void tp_mutex_lock(pthread_mutex_t *mutex)
{
  if (pthread_mutex_lock(mutex))
    abort();
}

void tp_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
  if (pthread_cond_wait(cond, mutex))
    abort();
}

void tp_mutex_unlock(pthread_mutex_t *mutex)
{
  if (pthread_mutex_unlock(mutex))
    abort();
}

/* On MacOS, threads other than the main thread are created with a reduced
 * stack size by default.  Adjust to RLIMIT_STACK aligned to the page size.
 *
 * On Linux, threads created by musl have a much smaller stack than threads
 * created by glibc (80 vs. 2048 or 4096 kB.)  Follow glibc for consistency.
 */
static size_t thread_stack_size(void)
{
#if defined(__APPLE__) || defined(__linux__)
  struct rlimit lim;

  if (getrlimit(RLIMIT_STACK, &lim))
    abort();

  if (lim.rlim_cur != RLIM_INFINITY)
  {
    /* pthread_attr_setstacksize() expects page-aligned values. */
    lim.rlim_cur -= lim.rlim_cur % (rlim_t)getpagesize();
    if (lim.rlim_cur >= PTHREAD_STACK_MIN)
      return lim.rlim_cur;
  }
#endif

#if !defined(__linux__)
  return 0;
#elif defined(__PPC__) || defined(__ppc__) || defined(__powerpc__)
  return 4 << 20; /* glibc default. */
#else
  return 2 << 20; /* glibc default. */
#endif
}

int tp_thread_create(pthread_t *tid, void (*entry)(void *), void *arg)
{
  int err;
  size_t stack_size;
  pthread_attr_t *attr;
  pthread_attr_t attr_storage;

  attr = NULL;
  stack_size = thread_stack_size();

  if (stack_size > 0)
  {
    attr = &attr_storage;

    if (pthread_attr_init(attr))
      abort();

    if (pthread_attr_setstacksize(attr, stack_size))
      abort();
  }

  err = pthread_create(tid, attr, (void *(*)(void *))entry, arg);

  if (attr != NULL)
    pthread_attr_destroy(attr);

  return err;
}

void tp_sem_wait(sem_t *sem)
{
  int r;

  do
    r = sem_wait(sem);
  while (r == -1 && errno == EINTR);

  if (r)
    abort();
}

void tp_sem_destroy(sem_t *sem)
{
  if (sem_destroy(sem))
    abort();
}

/* To avoid deadlock with tp_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */
static void worker(void *arg)
{
  struct tp_work *w;
  QUEUE *q;

  tp_sem_post((sem_t *)arg);
  arg = NULL;

  for (;;)
  {
    tp_mutex_lock(&mutex);

    while (QUEUE_EMPTY(&wq))
    {
      idle_threads += 1;
      tp_cond_wait(&cond, &mutex);
      idle_threads -= 1;
    }

    q = QUEUE_HEAD(&wq);

    if (q == &exit_message)
      pthread_cond_signal(&cond);
    else
    {
      QUEUE_REMOVE(q);
      QUEUE_INIT(q); /* Signal uv_cancel() that the work req is executing. */
    }

    tp_mutex_unlock(&mutex);

    if (q == &exit_message)
      break;

    w = QUEUE_DATA(q, struct tp_work, queue_node);
    w->work(w);
    w->work = NULL;
  }
}

static void post(QUEUE *q)
{
  tp_mutex_lock(&mutex);
  QUEUE_INSERT_TAIL(&wq, q);
  if (idle_threads > 0)
    pthread_cond_signal(&cond);
  tp_mutex_unlock(&mutex);
}

static void destory_threads()
{
  unsigned int i;

  if (nthreads == 0)
    return;

  post(&exit_message);

  for (i = 0; i < nthreads; i++)
    if (pthread_join(*(threads + i), NULL))
      abort();

  if (threads != default_threads)
    free(threads);

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

  threads = NULL;
  nthreads = 0;
}

static void init_threads(void)
{
  unsigned int i;
  const char *val;
  sem_t sem;

  nthreads = ARRAY_SIZE(default_threads);
  val = getenv("THREADPOOL_SIZE");
  if (val != NULL)
    nthreads = atoi(val);
  if (nthreads == 0)
    nthreads = 1;
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  threads = default_threads;
  if (nthreads > ARRAY_SIZE(default_threads))
  {
    threads = malloc(nthreads * sizeof(threads[0]));
    if (threads == NULL)
    {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }

  if (pthread_cond_init(&cond, NULL))
    abort();

  if (pthread_mutex_init(&mutex, NULL))
    abort();

  QUEUE_INIT(&wq);

  if (sem_init(&sem, 0, 0))
    abort();

  for (i = 0; i < nthreads; i++)
  {
    if (tp_thread_create(threads + i, worker, &sem))
      abort();
  }

  for (i = 0; i < nthreads; i++)
    tp_sem_wait(&sem);

  tp_sem_destroy(&sem);
}

static void reset_the_once(void)
{
  pthread_once_t child_once = PTHREAD_ONCE_INIT;
  memcpy(&once, &child_once, sizeof(child_once));
}

static void tp_init_once(void)
{
  if (pthread_atfork(NULL, NULL, &reset_the_once))
    abort();
  init_threads();
}

void tp_work_submit(struct tp_work *w,
                    void (*work)(struct tp_work *w),
                    void *data)
{
  pthread_once(&once, tp_init_once);
  w->work = work;
  w->data = data;
  post(&w->queue_node);
}

int tp_work_cancel(struct tp_work *w)
{
  int cancelled;

  tp_mutex_lock(&mutex);

  cancelled = !QUEUE_EMPTY(&w->queue_node) && w->work != NULL;
  if (cancelled)
    QUEUE_REMOVE(&w->queue_node);

  tp_mutex_unlock(&mutex);

  if (!cancelled)
    return -1;

  w->work = tp_cancelled;

  return 0;
}
