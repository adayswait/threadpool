#include <stdio.h>
#include <stdlib.h>
#include "threadpool.h"

#define WORKER_NUMBER 10000

void work(struct tp_work *w)
{
  printf("work get param %d\n", *((int *)(w->data)));
}
int main()
{
  setenv("THREADPOOL_SIZE", "100", 1);
  int n_arr[WORKER_NUMBER];
  struct tp_work w_arr[WORKER_NUMBER];
  int i;
  for (i = 0; i < WORKER_NUMBER; i++)
  {
    struct tp_work w;
    w_arr[i] = w;
    n_arr[i] = i;
    tp_work_submit(&w_arr[i], work, &n_arr[i]);
  }
  sleep(100);
  return 0;
}
