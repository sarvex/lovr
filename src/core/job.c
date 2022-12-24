#include "job.h"
#include <string.h>
#include <stdatomic.h>
#include <threads.h>

#define MAX_THREADS 64
#define MAX_JOBS 256
#define MAX_TASKS 256
#define TASK_MASK (MAX_TASKS - 1)

static void noop(job* job, void* arg) {
  //
}

struct job {
  uint32_t next;
  atomic_uint counter;
  fn_done* callback;
  void* arg;
};

typedef struct {
  task_info info;
  job* job;
} task;

static struct {
  uint32_t threadCount;
  thrd_t threads[MAX_THREADS];
  job jobs[MAX_JOBS];
  uint32_t nextJob;
  task tasks[MAX_TASKS];
  uint32_t head;
  uint32_t tail;
  mtx_t jobLock;
  mtx_t taskLock;
  cnd_t jobsDone;
  cnd_t taskPushed;
  cnd_t taskPopped;
  bool destroying;
} state;

static int worker_main(void* arg) {
  while (!state.destroying) {
    mtx_lock(&state.taskLock);

    while ((state.head & TASK_MASK) == (state.tail & TASK_MASK)) {
      cnd_wait(&state.taskPushed, &state.taskLock);

      if (state.destroying) {
        return 0;
      }
    }

    task task = state.tasks[state.tail++ & TASK_MASK];

    cnd_signal(&state.taskPopped);
    mtx_unlock(&state.taskLock);

    task.info.fn(task.job, task.info.arg);

    if (atomic_fetch_sub(&task.job->counter, 1) == 1) {
      mtx_lock(&state.jobLock);
      if (task.job->callback) {
        task.job->callback(task.job, task.job->arg);
        task.job->next = state.nextJob;
        state.nextJob = task.job - state.jobs;
      }
      cnd_signal(&state.jobsDone);
      mtx_unlock(&state.jobLock);
    }
  }

  return 0;
}

bool job_init(uint32_t workers) {
  if (workers > MAX_THREADS) {
    return false;
  }

  state.threadCount = workers;
  for (uint32_t i = 0; i < workers; i++) {
    if (thrd_create(&state.threads[i], worker_main, (void*) (uintptr_t) i) != thrd_success) {
      return false;
    }
  }

  state.nextJob = 0;
  state.jobs[MAX_JOBS - 1].next = ~0u;
  for (uint32_t i = 0; i < MAX_JOBS - 1; i++) {
    state.jobs[i].next = i + 1;
  }

  int err = thrd_success;
  err |= mtx_init(&state.jobLock, mtx_plain);
  err |= mtx_init(&state.taskLock, mtx_plain);
  err |= cnd_init(&state.jobsDone);
  err |= cnd_init(&state.taskPushed);
  err |= cnd_init(&state.taskPopped);
  return err == thrd_success;
}

void job_destroy(void) {
  state.destroying = true;
  cnd_broadcast(&state.taskPushed);
  for (uint32_t i = 0; i < state.threadCount; i++) {
    thrd_join(state.threads[i], NULL);
  }
  mtx_destroy(&state.jobLock);
  mtx_destroy(&state.taskLock);
  cnd_destroy(&state.jobsDone);
  cnd_destroy(&state.taskPushed);
  cnd_destroy(&state.taskPopped);
  memset(&state, 0, sizeof(state));
}

job* job_begin(void) {
  mtx_lock(&state.jobLock);
  if (state.nextJob == ~0u) {
    mtx_unlock(&state.jobLock);
    return NULL;
  }
  job* job = &state.jobs[state.nextJob];
  state.nextJob = job->next;
  mtx_unlock(&state.jobLock);
  job->next = ~0u;
  job->counter = 0;
  job->callback = NULL;
  job->arg = NULL;
  return job;
}

void job_add(job* job, task_info* tasks, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    mtx_lock(&state.taskLock);
    if (((state.head + 1) & TASK_MASK) == (state.tail & TASK_MASK)) {
      cnd_wait(&state.taskPopped, &state.taskLock);
    }
    job->counter++;
    state.tasks[state.head++ & TASK_MASK] = (task) { .info = tasks[i], .job = job };
    cnd_signal(&state.taskPushed);
    mtx_unlock(&state.taskLock);
  }
}

void job_commit(job* job, fn_done* callback, void* arg) {
  mtx_lock(&state.jobLock);
  job->callback = callback ? callback : noop;
  job->arg = arg;
  mtx_unlock(&state.jobLock);
}

void job_wait(job* job) {
  if (!job->callback) {
    return;
  }

  bool done = false;

  while (!done) {
    mtx_lock(&state.jobLock);

    done = job->counter == 0;

    if (!done) {
      cnd_wait(&state.jobsDone, &state.jobLock);
    }

    mtx_unlock(&state.jobLock);
  }
}
