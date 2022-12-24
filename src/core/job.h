#include <stdint.h>
#include <stdbool.h>

#pragma once

typedef struct job job;

typedef void fn_task(job* job, void* arg);
typedef void fn_done(job* job, void* arg);

typedef struct {
  fn_task* fn;
  void* arg;
} task_info;

bool job_init(uint32_t workers);
void job_destroy(void);
job* job_begin(void);
void job_add(job* job, task_info* tasks, uint32_t count);
void job_commit(job* job, fn_done* callback, void* arg);
void job_wait(job* job);
