#ifndef THREADPOOL_H_
#define THREADPOOL_H_

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define container_of(ptr, type, member) ({           \
 const typeof(((type *)0)->member) *__mptr = (ptr);  \
  (type *)((char *)__mptr - offsetof(type, member)); \
})

#include <semaphore.h>
#include <sys/resource.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

struct tp_work
{
  void (*work)(struct tp_work *w);
  void *data;
  void *queue_node[2];
};

void tp_work_submit(struct tp_work *w,
                    void (*work)(struct tp_work *w),
                    void *data);
int tp_work_cancel(struct tp_work *w);
#endif /* THREADPOOL_H_ */
