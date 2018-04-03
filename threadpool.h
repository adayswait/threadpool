#ifndef THREADPOOL_H_
#define THREADPOOL_H_

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

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
