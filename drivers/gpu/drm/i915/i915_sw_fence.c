/*
 * (C) Copyright 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/slab.h>
#include <linux/fence.h>
#include <linux/reservation.h>

#include "i915_sw_fence.h"

static DEFINE_SPINLOCK(i915_sw_fence_lock);

static int __i915_sw_fence_notify(struct i915_sw_fence *fence)
{
	i915_sw_fence_notify_t fn;

	fn = (i915_sw_fence_notify_t)(fence->flags & I915_SW_FENCE_MASK);
	return fn(fence);
}

static void i915_sw_fence_free(struct kref *kref)
{
	struct i915_sw_fence *fence = container_of(kref, typeof(*fence), kref);

	WARN_ON(atomic_read(&fence->pending) > 0);

	if (fence->flags & I915_SW_FENCE_MASK)
		WARN_ON(__i915_sw_fence_notify(fence) != NOTIFY_DONE);
	else
		kfree(fence);
}

static void i915_sw_fence_put(struct i915_sw_fence *fence)
{
	kref_put(&fence->kref, i915_sw_fence_free);
}

static struct i915_sw_fence *i915_sw_fence_get(struct i915_sw_fence *fence)
{
	kref_get(&fence->kref);
	return fence;
}

static void __i915_sw_fence_wake_up_all(struct i915_sw_fence *fence,
					struct list_head *continuation)
{
	wait_queue_head_t *x = &fence->wait;
	unsigned long flags;

	atomic_set(&fence->pending, -1); /* 0 -> -1 [done] */

	/*
	 * To prevent unbounded recursion as we traverse the graph of
	 * i915_sw_fences, we move the task_list from this the next ready
	 * fence to the tail of the original fence's task_list
	 * (and so added to the list to be woken).
	 */

	smp_mb__before_spinlock();
	spin_lock_irqsave_nested(&x->lock, flags, 1 + !!continuation);
	if (continuation) {
		list_splice_tail_init(&x->task_list, continuation);
	} else {
		LIST_HEAD(extra);

		do {
			__wake_up_locked_key(x, TASK_NORMAL, &extra);

			if (list_empty(&extra))
				break;

			list_splice_tail_init(&extra, &x->task_list);
		} while (1);
	}
	spin_unlock_irqrestore(&x->lock, flags);
}

static void __i915_sw_fence_complete(struct i915_sw_fence *fence,
				     struct list_head *continuation)
{
	if (!atomic_dec_and_test(&fence->pending))
		return;

	if (fence->flags & I915_SW_FENCE_MASK &&
	    __i915_sw_fence_notify(fence) != NOTIFY_DONE)
		return;

	__i915_sw_fence_wake_up_all(fence, continuation);
}

static void i915_sw_fence_complete(struct i915_sw_fence *fence)
{
	if (WARN_ON(i915_sw_fence_done(fence)))
		return;

	__i915_sw_fence_complete(fence, NULL);
}

static void i915_sw_fence_await(struct i915_sw_fence *fence)
{
	WARN_ON(atomic_inc_return(&fence->pending) <= 1);
}

static void i915_sw_fence_wait(struct i915_sw_fence *fence)
{
	wait_event(fence->wait, i915_sw_fence_done(fence));
}

void i915_sw_fence_init(struct i915_sw_fence *fence, i915_sw_fence_notify_t fn)
{
	BUG_ON((unsigned long)fn & ~I915_SW_FENCE_MASK);

	init_waitqueue_head(&fence->wait);
	kref_init(&fence->kref);
	atomic_set(&fence->pending, 1);
	fence->flags = (unsigned long)fn;
}

void i915_sw_fence_commit(struct i915_sw_fence *fence)
{
	i915_sw_fence_complete(fence);
	i915_sw_fence_put(fence);
}

static int i915_sw_fence_wake(wait_queue_t *wq, unsigned mode, int flags, void *key)
{
	list_del(&wq->task_list);
	__i915_sw_fence_complete(wq->private, key);
	i915_sw_fence_put(wq->private);
	kfree(wq);
	return 0;
}

static bool __i915_sw_fence_check_if_after(struct i915_sw_fence *fence,
				    const struct i915_sw_fence * const signaler)
{
	wait_queue_t *wq;

	if (__test_and_set_bit(I915_SW_FENCE_CHECKED_BIT, &fence->flags))
		return false;

	if (fence == signaler)
		return true;

	list_for_each_entry(wq, &fence->wait.task_list, task_list) {
		if (wq->func != i915_sw_fence_wake)
			continue;

		if (__i915_sw_fence_check_if_after(wq->private, signaler))
			return true;
	}

	return false;
}

static void __i915_sw_fence_clear_checked_bit(struct i915_sw_fence *fence)
{
	wait_queue_t *wq;

	if (!__test_and_clear_bit(I915_SW_FENCE_CHECKED_BIT, &fence->flags))
		return;

	list_for_each_entry(wq, &fence->wait.task_list, task_list) {
		if (wq->func != i915_sw_fence_wake)
			continue;

		__i915_sw_fence_clear_checked_bit(wq->private);
	}
}

static bool i915_sw_fence_check_if_after(struct i915_sw_fence *fence,
				  const struct i915_sw_fence * const signaler)
{
	unsigned long flags;
	bool err;

	if (!IS_ENABLED(CONFIG_I915_SW_FENCE_CHECK_DAG))
		return false;

	spin_lock_irqsave(&i915_sw_fence_lock, flags);
	err = __i915_sw_fence_check_if_after(fence, signaler);
	__i915_sw_fence_clear_checked_bit(fence);
	spin_unlock_irqrestore(&i915_sw_fence_lock, flags);

	return err;
}

static wait_queue_t *__i915_sw_fence_create_wq(struct i915_sw_fence *fence, gfp_t gfp)
{
	wait_queue_t *wq;

	wq = kmalloc(sizeof(*wq), gfp);
	if (unlikely(!wq))
		return NULL;

	INIT_LIST_HEAD(&wq->task_list);
	wq->flags = 0;
	wq->func = i915_sw_fence_wake;
	wq->private = i915_sw_fence_get(fence);

	i915_sw_fence_await(fence);

	return wq;
}

int i915_sw_fence_await_sw_fence(struct i915_sw_fence *fence,
				 struct i915_sw_fence *signaler,
				 gfp_t gfp)
{
	wait_queue_t *wq;
	unsigned long flags;
	int pending;

	if (i915_sw_fence_done(signaler))
		return 0;

	/* The dependency graph must be acyclic. */
	if (unlikely(i915_sw_fence_check_if_after(fence, signaler)))
		return -EINVAL;

	wq = __i915_sw_fence_create_wq(fence, gfp);
	if (unlikely(!wq)) {
		if (!gfpflags_allow_blocking(gfp))
			return -ENOMEM;

		i915_sw_fence_wait(signaler);
		return 0;
	}

	spin_lock_irqsave(&signaler->wait.lock, flags);
	if (likely(!i915_sw_fence_done(signaler))) {
		__add_wait_queue_tail(&signaler->wait, wq);
		pending = 1;
	} else {
		i915_sw_fence_wake(wq, 0, 0, NULL);
		pending = 0;
	}
	spin_unlock_irqrestore(&signaler->wait.lock, flags);

	return pending;
}

struct dma_fence_cb {
	struct fence_cb base;
	struct i915_sw_fence *fence;
};

static void dma_i915_sw_fence_wake(struct fence *dma, struct fence_cb *data)
{
	struct dma_fence_cb *cb = container_of(data, typeof(*cb), base);

	i915_sw_fence_commit(cb->fence);
	kfree(cb);
}

int i915_sw_fence_await_dma_fence(struct i915_sw_fence *fence,
				  struct fence *dma, gfp_t gfp)
{
	struct dma_fence_cb *cb;
	int ret;

	if (fence_is_signaled(dma))
		return 0;

	cb = kmalloc(sizeof(*cb), gfp);
	if (!cb) {
		if (!gfpflags_allow_blocking(gfp))
			return -ENOMEM;

		return fence_wait(dma, false);
	}

	cb->fence = i915_sw_fence_get(fence);
	i915_sw_fence_await(fence);

	ret = fence_add_callback(dma, &cb->base, dma_i915_sw_fence_wake);
	if (ret == 0) {
		ret = 1;
	} else {
		dma_i915_sw_fence_wake(dma, &cb->base);
		if (ret == -ENOENT) /* fence already signaled */
			ret = 0;
	}

	return ret;
}

int i915_sw_fence_await_reservation(struct i915_sw_fence *fence,
				    struct reservation_object *resv,
				    const struct fence_ops *exclude,
				    bool write,
				    gfp_t gfp)
{
	struct fence *excl, **shared;
	unsigned int count, i;
	int ret;

	ret = reservation_object_get_fences_rcu(resv, &excl, &count, &shared);
	if (ret)
		return ret;

	if (write) {
		for (i = 0; i < count; i++) {
			int pending;

			if (shared[i]->ops == exclude)
				continue;

			pending = i915_sw_fence_await_dma_fence(fence,
								shared[i],
								gfp);
			if (pending < 0) {
				ret = pending;
				goto out;
			}

			ret |= pending;
		}
	}

	if (excl && excl->ops != exclude) {
		int pending;

		pending = i915_sw_fence_await_dma_fence(fence, excl, gfp);
		if (pending < 0) {
			ret = pending;
			goto out;
		}

		ret |= pending;
	}

out:
	fence_put(excl);
	for (i = 0; i < count; i++)
		fence_put(shared[i]);
	kfree(shared);

	return ret;
}
