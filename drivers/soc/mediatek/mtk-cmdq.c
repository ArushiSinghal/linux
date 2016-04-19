/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <soc/mediatek/cmdq.h>

#define CMDQ_MAX_THREAD_COUNT		3 /* general, main, sub */
#define CMDQ_MAX_TASK_IN_THREAD		2

#define CMDQ_INITIAL_CMD_BLOCK_SIZE	PAGE_SIZE
#define CMDQ_INST_SIZE			8 /* instruction is 64-bit */

/*
 * cmdq_thread cookie value is from 0 to CMDQ_MAX_COOKIE_VALUE.
 * And, this value also be used as MASK.
 */
#define CMDQ_MAX_COOKIE_VALUE		0xffff
#define CMDQ_COOKIE_MASK		CMDQ_MAX_COOKIE_VALUE

#define CMDQ_DEFAULT_TIMEOUT_MS		1000
#define CMDQ_ACQUIRE_THREAD_TIMEOUT_MS	5000

#define CMDQ_DRIVER_DEVICE_NAME		"mtk_cmdq"

#define CMDQ_CLK_NAME			"gce"

#define CMDQ_CURR_IRQ_STATUS_OFFSET	0x010
#define CMDQ_CURR_LOADED_THR_OFFSET	0x018
#define CMDQ_THR_SLOT_CYCLES_OFFSET	0x030

#define CMDQ_THR_BASE			0x100
#define CMDQ_THR_SHIFT			0x080
#define CMDQ_THR_WARM_RESET_OFFSET	0x00
#define CMDQ_THR_ENABLE_TASK_OFFSET	0x04
#define CMDQ_THR_SUSPEND_TASK_OFFSET	0x08
#define CMDQ_THR_CURR_STATUS_OFFSET	0x0c
#define CMDQ_THR_IRQ_STATUS_OFFSET	0x10
#define CMDQ_THR_IRQ_ENABLE_OFFSET	0x14
#define CMDQ_THR_CURR_ADDR_OFFSET	0x20
#define CMDQ_THR_END_ADDR_OFFSET	0x24
#define CMDQ_THR_EXEC_CNT_OFFSET	0x28
#define CMDQ_THR_CFG_OFFSET		0x40
#define CMDQ_THR_INST_CYCLES_OFFSET	0x50

#define CMDQ_SYNC_TOKEN_SET		BIT(16)
#define CMDQ_IRQ_MASK			0xffff

#define CMDQ_THR_ENABLED		0x1
#define CMDQ_THR_DISABLED		0x0
#define CMDQ_THR_SUSPEND		0x1
#define CMDQ_THR_RESUME			0x0
#define CMDQ_THR_STATUS_SUSPENDED	BIT(1)
#define CMDQ_THR_WARM_RESET		BIT(0)
#define CMDQ_THR_SLOT_CYCLES		0x3200
#define CMDQ_THR_NO_TIMEOUT		0x0
#define CMDQ_THR_PRIORITY		3
#define CMDQ_THR_IRQ_DONE		0x1
#define CMDQ_THR_IRQ_ERROR		0x12
#define CMDQ_THR_IRQ_EN			0x13 /* done + error */
#define CMDQ_THR_IRQ_MASK		0x13
#define CMDQ_THR_EXECUTING		BIT(31)

#define CMDQ_ARG_A_MASK			0xffffff
#define CMDQ_ARG_A_WRITE_MASK		0xffff
#define CMDQ_ARG_A_SUBSYS_MASK		0x1f0000
#define CMDQ_SUBSYS_MASK		0x1f

#define CMDQ_OP_CODE_SHIFT		24
#define CMDQ_SUBSYS_SHIFT		16

#define CMDQ_JUMP_BY_OFFSET		0x10000000
#define CMDQ_JUMP_BY_PA			0x10000001
#define CMDQ_JUMP_TO_BEGIN		0x8

#define CMDQ_WFE_UPDATE			BIT(31)
#define CMDQ_WFE_WAIT			BIT(15)
#define CMDQ_WFE_WAIT_VALUE		0x1

#define CMDQ_EOC_IRQ_EN			BIT(0)

#define CMDQ_ENABLE_MASK		BIT(0)

struct cmdq_command {
	struct cmdq	*cmdq;
	u64		engine_flag;
	void		*base; /* command buffer pointer */
	size_t		size; /* command buffer size (bytes) */
};

/*
 * [HW OP code]
 * CMDQ_CODE_MOVE:
 *   move value into internal register as mask
 *   format: op mask
 * CMDQ_CODE_WRITE:
 *   write value into target register
 *   format: op subsys address value
 * CMDQ_CODE_JUMP:
 *   jump by offset
 *   format: op offset
 * CMDQ_CODE_WFE:
 *   wait for event and clear
 *   it is just clear if no wait
 *   format: [wait]  op event update:1 to_wait:1 wait:1
 *           [clear] op event update:1 to_wait:0 wait:0
 * CMDQ_CODE_EOC:
 *   end of command
 *   format: op irq_flag
 *
 * [SW OP code]
 * CMDQ_CODE_CLEAR_EVENT:
 *   redirect to CMDQ_CODE_WFE
 *
 * Please see cmdq_rec_append_command() for details.
 */
enum cmdq_code {
	CMDQ_CODE_MOVE = 0x02,
	CMDQ_CODE_WRITE = 0x04,
	CMDQ_CODE_JUMP = 0x10,
	CMDQ_CODE_WFE = 0x20,
	CMDQ_CODE_CLEAR_EVENT = 0x21,
	CMDQ_CODE_EOC = 0x40,
};

enum cmdq_task_state {
	TASK_STATE_IDLE,	/* free task */
	TASK_STATE_BUSY,	/* task running on a thread */
	TASK_STATE_KILLED,	/* task process being killed */
	TASK_STATE_ERROR,	/* task execution error */
	TASK_STATE_START_ERROR,	/* fail to start task execution */
	TASK_STATE_DONE,	/* task finished */
	TASK_STATE_WAITING,	/* allocated but waiting for available thread */
};

struct cmdq_task_cb {
	cmdq_async_flush_cb	cb;
	void			*data;
};

struct cmdq_thread;

struct cmdq_task {
	struct cmdq		*cmdq;
	struct list_head	list_entry;

	/* state for task life cycle */
	enum cmdq_task_state	task_state;
	/* virtual address of command buffer */
	void			*va_base;
	/* physical address of command buffer */
	dma_addr_t		mva_base;
	/* size of allocated command buffer */
	size_t			buf_size;

	u64			engine_flag;
	size_t			command_size;
	u32			num_cmd; /* 2 * number of commands */
	struct cmdq_thread	*thread;
	/* callback functions */
	struct cmdq_task_cb	cb;
	/* work item when auto release is used */
	struct work_struct	auto_release_work;
};

struct cmdq_thread {
	int			id;
	void __iomem		*base;
	u32			task_count;
	u32			wait_cookie;
	u32			next_cookie;
	struct cmdq_task	*cur_task[CMDQ_MAX_TASK_IN_THREAD];
	wait_queue_head_t	wait_queue; /* wait task done */
};

struct cmdq {
	struct device		*dev;
	struct notifier_block	pm_notifier;

	void __iomem		*base;
	u32			irq;

	/*
	 * task information
	 * task_cache: struct cmdq_task object cache
	 * task_active_list: active tasks
	 * task_consume_wait_queue_item: task consumption work item
	 * task_auto_release_wq: auto-release workqueue
	 * task_consume_wq: task consumption workqueue (for queued tasks)
	 */
	struct kmem_cache	*task_cache;
	struct list_head	task_active_list;
	struct list_head	task_wait_list;
	struct work_struct	task_consume_wait_queue_item;
	struct workqueue_struct	*task_auto_release_wq;
	struct workqueue_struct	*task_consume_wq;

	struct cmdq_thread	thread[CMDQ_MAX_THREAD_COUNT];

	/* mutex, spinlock, flag */
	struct mutex		task_mutex;	/* for task list */
	spinlock_t		thread_lock;	/* for cmdq hardware thread */
	atomic_t		thread_usage;
	spinlock_t		exec_lock;	/* for exec task */

	/* suspend */
	atomic_t		suspending;
	bool			suspended;

	/* wait thread acquiring */
	wait_queue_head_t	thread_dispatch_queue;

	/* ccf */
	struct clk		*clock;
};

struct cmdq_subsys {
	u32	base_addr;
	int	id;
};

static const struct cmdq_subsys g_subsys[] = {
	{0x1400, 1},
	{0x1401, 2},
	{0x1402, 3},
};

static int cmdq_clk_enable(struct cmdq *cmdq)
{
	struct device *dev = cmdq->dev;
	int err;

	err = clk_prepare_enable(cmdq->clock);
	if (err) {
		dev_err(dev, "prepare and enable clk:%s fail\n",
			CMDQ_CLK_NAME);
		return err;
	}

	return 0;
}

static void cmdq_clk_disable(struct cmdq *cmdq)
{
	clk_disable_unprepare(cmdq->clock);
}

static int cmdq_subsys_base_addr_to_id(u32 base_addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(g_subsys); i++)
		if (g_subsys[i].base_addr == base_addr)
			return g_subsys[i].id;

	return -EFAULT;
}

static int cmdq_eng_get_thread(u64 flag)
{
	if (flag & BIT_ULL(CMDQ_ENG_DISP_DSI0))
		return 0;
	else if (flag & BIT_ULL(CMDQ_ENG_DISP_DPI0))
		return 1;
	else
		return 2;
}

/*
 * It's a kmemcache creator for cmdq_task to initialize variables
 * without command buffer.
 */
static void cmdq_task_ctor(void *param)
{
	struct cmdq_task *task = param;

	memset(task, 0, sizeof(*task));
	INIT_LIST_HEAD(&task->list_entry);
	task->task_state = TASK_STATE_IDLE;
	task->thread = NULL;
}

static void cmdq_task_free_command_buffer(struct cmdq_task *task)
{
	if (!task->va_base)
		return;

	dma_free_coherent(task->cmdq->dev, task->buf_size, task->va_base,
			  task->mva_base);

	task->va_base = NULL;
	task->mva_base = 0;
	task->buf_size = 0;
	task->command_size = 0;
	task->num_cmd = 0;
}

/*
 * Ensure size of command buffer in the given cmdq_task.
 * Existing buffer data will be copied to new buffer.
 * This buffer is guaranteed to be physically continuous.
 * returns -ENOMEM if cannot allocate new buffer
 */
static int cmdq_task_realloc_command_buffer(struct cmdq_task *task, size_t size)
{
	struct device *dev = task->cmdq->dev;
	void *new_buf;
	dma_addr_t new_mva_base;
	size_t cmd_size;
	u32 num_cmd;

	if (task->va_base && task->buf_size >= size)
		return 0;

	new_buf = dma_alloc_coherent(dev, size, &new_mva_base,
				     GFP_KERNEL);
	if (!new_buf) {
		dev_err(dev, "alloc cmd buffer of size %zu failed\n", size);
		return -ENOMEM;
	}

	/* copy and release old buffer */
	if (task->va_base)
		memcpy(new_buf, task->va_base, task->buf_size);

	/*
	 * we should keep track of num_cmd and cmd_size
	 * since they are cleared in free command buffer
	 */
	num_cmd = task->num_cmd;
	cmd_size = task->command_size;
	cmdq_task_free_command_buffer(task);

	/* attach the new buffer */
	task->va_base = new_buf;
	task->mva_base = new_mva_base;
	task->buf_size = size;
	task->num_cmd = num_cmd;
	task->command_size = cmd_size;

	return 0;
}

/* allocate and initialize struct cmdq_task and its command buffer */
static struct cmdq_task *cmdq_task_create(struct cmdq *cmdq)
{
	struct device *dev = cmdq->dev;
	struct cmdq_task *task;
	int err;

	mutex_lock(&cmdq->task_mutex);
	task = kmem_cache_alloc(cmdq->task_cache, GFP_KERNEL);
	task->cmdq = cmdq;
	err = cmdq_task_realloc_command_buffer(
			task, CMDQ_INITIAL_CMD_BLOCK_SIZE);
	if (err < 0) {
		dev_err(dev, "allocate command buffer failed\n");
		kmem_cache_free(cmdq->task_cache, task);
		task = NULL;
	}
	mutex_unlock(&cmdq->task_mutex);
	return task;
}

static void cmdq_task_release_unlocked(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;

	/* This func should be inside cmdq->task_mutex mutex */
	lockdep_assert_held(&cmdq->task_mutex);

	cmdq_task_free_command_buffer(task);
	list_del(&task->list_entry);
	kmem_cache_free(cmdq->task_cache, task);
}

static void cmdq_task_release_internal(struct cmdq_task *task)
{
	mutex_lock(&task->cmdq->task_mutex);
	cmdq_task_release_unlocked(task);
	mutex_unlock(&task->cmdq->task_mutex);
}

/* After dropping error task, we have to reorder remaining valid tasks. */
static void cmdq_thread_reorder_task_array(struct cmdq_thread *thread,
					   int prev_id)
{
	int to_id, from_id;
	struct cmdq_task *task;
	u32 *task_base;

	to_id = (prev_id + 1) % CMDQ_MAX_TASK_IN_THREAD;
	if (thread->cur_task[to_id])
		return;

	thread->next_cookie--;
	from_id = (to_id + 1) % CMDQ_MAX_TASK_IN_THREAD;
	for (; from_id != prev_id;
	     from_id = (from_id + 1) % CMDQ_MAX_TASK_IN_THREAD) {
		if (!thread->cur_task[from_id]) {
			thread->next_cookie--;
			continue;
		}
		thread->cur_task[to_id] = thread->cur_task[from_id];
		thread->cur_task[from_id] = NULL;
		task = thread->cur_task[to_id];
		task_base = task->va_base;
		if ((task_base[task->num_cmd - 1] == CMDQ_JUMP_BY_OFFSET) &&
		    (task_base[task->num_cmd - 2] == CMDQ_JUMP_TO_BEGIN)) {
			break; /* reach the last task */
		}
		to_id = (to_id + 1) % CMDQ_MAX_TASK_IN_THREAD;
	}
}

static int cmdq_task_fill_command(struct cmdq_task *task,
				  struct cmdq_command *command)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	int err;

	err = cmdq_task_realloc_command_buffer(task, task->command_size);
	if (err < 0) {
		dev_err(dev, "task(0x%p) failed to realloc command buffer\n",
			task);
		return err;
	}

	memcpy(task->va_base, command->base, command->size);
	task->num_cmd = task->command_size / sizeof(u32);
	return 0;
}

static struct cmdq_task *cmdq_task_acquire(struct cmdq_command *command,
					   struct cmdq_task_cb *cb)
{
	struct cmdq *cmdq = command->cmdq;
	struct device *dev = cmdq->dev;
	struct cmdq_task *task;

	task = cmdq_task_create(cmdq);
	if (!task) {
		dev_err(dev, "can't acquire task info\n");
		return NULL;
	}

	/* initialize field values */
	task->engine_flag = command->engine_flag;
	task->task_state = TASK_STATE_WAITING;
	if (cb)
		task->cb = *cb;
	else
		memset(&task->cb, 0, sizeof(task->cb));
	task->command_size = command->size;

	if (cmdq_task_fill_command(task, command) < 0) {
		dev_err(dev, "fail to sync command\n");
		cmdq_task_release_internal(task);
		return NULL;
	}

	/* insert into waiting list to process */
	mutex_lock(&cmdq->task_mutex);
	list_add_tail(&task->list_entry, &cmdq->task_wait_list);
	mutex_unlock(&cmdq->task_mutex);

	return task;
}

static void cmdq_thread_writel(struct cmdq_thread *thread, u32 value,
			       u32 offset)
{
	writel(value, thread->base + offset);
}

static u32 cmdq_thread_readl(struct cmdq_thread *thread, u32 offset)
{
	return readl(thread->base + offset);
}

static u32 cmdq_thread_get_cookie(struct cmdq_thread *thread)
{
	return cmdq_thread_readl(thread, CMDQ_THR_EXEC_CNT_OFFSET) &
				 CMDQ_COOKIE_MASK;
}

static struct cmdq_thread *cmdq_thread_get(struct cmdq *cmdq, u64 flag)
{
	int tid = cmdq_eng_get_thread(flag);
	struct cmdq_thread *thread = &cmdq->thread[tid];
	u32 next_cookie;

	next_cookie = thread->next_cookie % CMDQ_MAX_TASK_IN_THREAD;
	if (thread->cur_task[next_cookie])
		return NULL;

	cmdq_clk_enable(cmdq);
	atomic_inc(&cmdq->thread_usage);
	return thread;
}

static void cmdq_thread_put(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	if (WARN_ON(thread == NULL))
		return;

	cmdq_clk_disable(cmdq);
	atomic_dec(&cmdq->thread_usage);
}

static int cmdq_thread_suspend(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	u32 enabled;
	u32 status;

	/* write suspend bit */
	cmdq_thread_writel(thread, CMDQ_THR_SUSPEND,
			   CMDQ_THR_SUSPEND_TASK_OFFSET);

	/* If already disabled, treat as suspended successful. */
	enabled = cmdq_thread_readl(thread, CMDQ_THR_ENABLE_TASK_OFFSET);
	if (!(enabled & CMDQ_THR_ENABLED))
		return 0;

	/* poll suspended status */
	if (readl_poll_timeout_atomic(thread->base +
				      CMDQ_THR_CURR_STATUS_OFFSET,
				      status,
				      status & CMDQ_THR_STATUS_SUSPENDED,
				      0, 10)) {
		dev_err(cmdq->dev, "Suspend HW thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}

	return 0;
}

static void cmdq_thread_resume(struct cmdq_thread *thread)
{
	cmdq_thread_writel(thread, CMDQ_THR_RESUME,
			   CMDQ_THR_SUSPEND_TASK_OFFSET);
}

static int cmdq_thread_reset(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	void __iomem *gce_base = cmdq->base;
	u32 warm_reset;

	cmdq_thread_writel(thread, CMDQ_THR_WARM_RESET,
			   CMDQ_THR_WARM_RESET_OFFSET);

	if (readl_poll_timeout_atomic(thread->base + CMDQ_THR_WARM_RESET_OFFSET,
				      warm_reset,
				      !(warm_reset & CMDQ_THR_WARM_RESET),
				      0, 10)) {
		dev_err(cmdq->dev, "Reset HW thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}

	writel(CMDQ_THR_SLOT_CYCLES, gce_base + CMDQ_THR_SLOT_CYCLES_OFFSET);
	return 0;
}

static void cmdq_thread_disable(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	cmdq_thread_reset(cmdq, thread);
	cmdq_thread_writel(thread, CMDQ_THR_DISABLED,
			   CMDQ_THR_ENABLE_TASK_OFFSET);
}

static void cmdq_thread_insert_task_by_cookie(struct cmdq_thread *thread,
					      struct cmdq_task *task,
					      int cookie)
{
	thread->wait_cookie = cookie;
	thread->next_cookie = cookie + 1;
	if (thread->next_cookie > CMDQ_MAX_COOKIE_VALUE)
		thread->next_cookie = 0;

	/* first task, so set to 1 */
	thread->task_count = 1;

	thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD] = task;
}

static int cmdq_thread_remove_task_by_index(struct cmdq_thread *thread,
		int index, enum cmdq_task_state new_state)
{
	struct cmdq *cmdq;
	struct cmdq_task *task;
	struct device *dev;

	task = thread->cur_task[index];
	if (!task) {
		pr_err("%s: remove fail, task:%d on thread:0x%p is NULL\n",
		       __func__, index, thread);
		return -EINVAL;
	}
	cmdq = task->cmdq;
	dev = cmdq->dev;

	task->task_state = new_state;
	thread->cur_task[index] = NULL;
	thread->task_count--;
	return 0;
}

static int cmdq_thread_force_remove_task(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct cmdq_thread *thread = task->thread;
	int status;
	int cookie;
	struct cmdq_task *exec_task;

	status = cmdq_thread_suspend(cmdq, thread);

	cmdq_thread_writel(thread, CMDQ_THR_NO_TIMEOUT,
			   CMDQ_THR_INST_CYCLES_OFFSET);

	/* The cookie of the task currently being processed */
	cookie = cmdq_thread_get_cookie(thread) + 1;

	exec_task = thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD];
	if (exec_task == task) {
		dma_addr_t eoc_pa = task->mva_base + task->command_size - 16;

		/* The task is executed now, set the PC to EOC for bypass */
		cmdq_thread_writel(thread, eoc_pa, CMDQ_THR_CURR_ADDR_OFFSET);

		thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD] = NULL;
		task->task_state = TASK_STATE_KILLED;
	} else {
		int i, j;
		u32 *task_base, *exec_task_base;

		j = thread->task_count;
		for (i = cookie; j > 0; j--, i++) {
			i %= CMDQ_MAX_TASK_IN_THREAD;

			exec_task = thread->cur_task[i];
			if (!exec_task)
				continue;

			task_base = task->va_base;
			exec_task_base = exec_task->va_base;
			if ((exec_task_base[exec_task->num_cmd - 1] ==
			     CMDQ_JUMP_BY_OFFSET) &&
			    (exec_task_base[exec_task->num_cmd - 2] ==
			     CMDQ_JUMP_TO_BEGIN)) {
				/* reached the last task */
				break;
			}

			if (exec_task_base[exec_task->num_cmd - 2] ==
			    task->mva_base) {
				/* fake EOC command */
				exec_task_base[exec_task->num_cmd - 2] =
					CMDQ_EOC_IRQ_EN;
				exec_task_base[exec_task->num_cmd - 1] =
					CMDQ_CODE_EOC << CMDQ_OP_CODE_SHIFT;

				/* bypass the task */
				exec_task_base[exec_task->num_cmd] =
					task_base[task->num_cmd - 2];
				exec_task_base[exec_task->num_cmd + 1] =
					task_base[task->num_cmd - 1];

				i = (i + 1) % CMDQ_MAX_TASK_IN_THREAD;

				thread->cur_task[i] = NULL;
				task->task_state = TASK_STATE_KILLED;
				status = 0;
				break;
			}
		}
	}

	return status;
}

static struct cmdq_task *cmdq_thread_search_task_by_pc(
		const struct cmdq_thread *thread, u32 pc)
{
	struct cmdq_task *task;
	int i;

	for (i = 0; i < CMDQ_MAX_TASK_IN_THREAD; i++) {
		task = thread->cur_task[i];
		if (task &&
		    pc >= task->mva_base &&
		    pc < task->mva_base + task->command_size)
			break;
	}

	return task;
}

/*
 * Re-fetch thread's command buffer
 * Use Case:
 *     If SW modifies command buffer content after SW configed commands to GCE,
 *     SW should notify GCE to re-fetch commands in order to
 *     prevent inconsistent command buffer content between DRAM and GCE's SRAM.
 */
static void cmdq_thread_invalidate_fetched_data(struct cmdq_thread *thread)
{
	u32 pc;

	/*
	 * Setting HW thread PC will invoke that
	 * GCE (CMDQ HW) gives up fetched command buffer,
	 * and fetch command from DRAM to GCE's SRAM again.
	 */
	pc = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);
	cmdq_thread_writel(thread, pc, CMDQ_THR_CURR_ADDR_OFFSET);
}

static int cmdq_task_insert_into_thread(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	struct cmdq_thread *thread = task->thread;
	struct cmdq_task *prev_task;
	int index, prev, curr;
	int loop = CMDQ_MAX_TASK_IN_THREAD - 1;
	u32 *prev_task_base;

	/* find previous task */

	index = thread->next_cookie % CMDQ_MAX_TASK_IN_THREAD;
	prev = (index + CMDQ_MAX_TASK_IN_THREAD - 1) % CMDQ_MAX_TASK_IN_THREAD;
	prev_task = thread->cur_task[prev];

	/* maybe the job is killed, search a new one */
	for (; !prev_task && loop > 1; loop--) {
		dev_err(dev,
			"prev_task is NULL, prev:%d, loop:%d, index:%d\n",
			prev, loop, index);

		prev--;
		if (prev < 0)
			prev = CMDQ_MAX_TASK_IN_THREAD - 1;

		prev_task = thread->cur_task[prev];
	}

	if (!prev_task) {
		dev_err(dev,
			"invalid prev_task index:%d, loop:%d\n",
			index, loop);
		return -EFAULT;
	}

	/* compare with current running task */
	curr = (cmdq_thread_get_cookie(thread) + 1) % CMDQ_MAX_TASK_IN_THREAD;
	if ((index > prev && (curr > prev && curr <= index)) ||
	    (index < prev && (curr > prev || curr <= index))) {
		dev_err(dev,
			"invalid prev_task: index:%d, prev:%d, curr:%d\n",
			index, prev, curr);
		return -EFAULT;
	}

	/* insert this task */
	thread->cur_task[index] = task;
	/* let previous task jump to this new task */
	prev_task_base = prev_task->va_base;
	prev_task_base[prev_task->num_cmd - 1] = CMDQ_JUMP_BY_PA;
	prev_task_base[prev_task->num_cmd - 2] = task->mva_base;

	/* re-fetch command buffer again. */
	cmdq_thread_invalidate_fetched_data(thread);

	return 0;
}

static int cmdq_task_exec_async(struct cmdq_task *task,
				struct cmdq_thread *thread)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	unsigned long flags;
	int minimum;
	int cookie;

	spin_lock_irqsave(&cmdq->exec_lock, flags);

	/* update task's thread info */
	task->thread = thread;
	task->task_state = TASK_STATE_BUSY;

	if (thread->task_count <= 0) {
		if (cmdq_thread_reset(cmdq, thread) < 0) {
			spin_unlock_irqrestore(&cmdq->exec_lock, flags);
			return -EFAULT;
		}

		cmdq_thread_writel(thread, CMDQ_THR_NO_TIMEOUT,
				   CMDQ_THR_INST_CYCLES_OFFSET);
		cmdq_thread_writel(thread, task->mva_base,
				   CMDQ_THR_CURR_ADDR_OFFSET);
		cmdq_thread_writel(thread, task->mva_base + task->command_size,
				   CMDQ_THR_END_ADDR_OFFSET);
		cmdq_thread_writel(thread, CMDQ_THR_PRIORITY,
				   CMDQ_THR_CFG_OFFSET);
		cmdq_thread_writel(thread, CMDQ_THR_IRQ_EN,
				   CMDQ_THR_IRQ_ENABLE_OFFSET);

		minimum = cmdq_thread_get_cookie(thread);
		cmdq_thread_insert_task_by_cookie(
				thread, task, (minimum + 1));

		/* enable HW thread */
		cmdq_thread_writel(thread, CMDQ_THR_ENABLED,
				   CMDQ_THR_ENABLE_TASK_OFFSET);
	} else {
		unsigned long curr_pa, end_pa;
		int err;

		err = cmdq_thread_suspend(cmdq, thread);
		if (err < 0) {
			spin_unlock_irqrestore(&cmdq->exec_lock, flags);
			return err;
		}

		cmdq_thread_writel(thread, CMDQ_THR_NO_TIMEOUT,
				   CMDQ_THR_INST_CYCLES_OFFSET);

		cookie = thread->next_cookie;

		/*
		 * Boundary case tested: EOC have been executed,
		 *                       but JUMP is not executed
		 * Thread PC: 0x9edc0dd8, End: 0x9edc0de0,
		 * Curr Cookie: 1, Next Cookie: 2
		 * PC = END - 8, EOC is executed
		 * PC = END - 0, All CMDs are executed
		 */

		curr_pa = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);
		end_pa = cmdq_thread_readl(thread, CMDQ_THR_END_ADDR_OFFSET);
		if ((curr_pa == end_pa - 8) || (curr_pa == end_pa - 0)) {
			/* set to task directly */
			cmdq_thread_writel(thread, task->mva_base,
					   CMDQ_THR_CURR_ADDR_OFFSET);
			cmdq_thread_writel(thread,
					   task->mva_base + task->command_size,
					   CMDQ_THR_END_ADDR_OFFSET);
			thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD] =
					task;
			thread->task_count++;
		} else {
			err = cmdq_task_insert_into_thread(task);
			if (err < 0) {
				spin_unlock_irqrestore(
						&cmdq->exec_lock, flags);
				dev_err(dev, "insert task failed\n");
				return err;
			}

			smp_mb(); /* modify jump before enable thread */

			cmdq_thread_writel(thread,
					   task->mva_base + task->command_size,
					   CMDQ_THR_END_ADDR_OFFSET);
			thread->task_count++;
		}

		thread->next_cookie++;
		if (thread->next_cookie > CMDQ_MAX_COOKIE_VALUE)
			thread->next_cookie = 0;

		/* resume HW thread */
		cmdq_thread_resume(thread);
	}

	spin_unlock_irqrestore(&cmdq->exec_lock, flags);

	return 0;
}

static void cmdq_handle_done_internal(struct cmdq *cmdq,
				      struct cmdq_thread *thread,
				      int cookie)
{
	int i, new_wait_cookie;
	struct cmdq_task *task;
	struct cmdq_cb_data cmdq_cb_data;

	new_wait_cookie = (cookie + 1) % (CMDQ_MAX_COOKIE_VALUE + 1);
	for (i = thread->wait_cookie % CMDQ_MAX_TASK_IN_THREAD;
	     i != new_wait_cookie % CMDQ_MAX_TASK_IN_THREAD;
	     i = (i + 1) % CMDQ_MAX_TASK_IN_THREAD) {
		if (thread->cur_task[i]) {
			task = thread->cur_task[i];
			if (task->cb.cb) {
				cmdq_cb_data.err = false;
				cmdq_cb_data.data = task->cb.data;
				task->cb.cb(cmdq_cb_data);
			}
			cmdq_thread_remove_task_by_index(
					thread, i, TASK_STATE_DONE);
		}
	}
	thread->wait_cookie = new_wait_cookie;
	wake_up(&thread->wait_queue);
}

static void cmdq_handle_error(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	struct device *dev = cmdq->dev;
	u32 curr_pa = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);
	u32 end_pa = cmdq_thread_readl(thread, CMDQ_THR_END_ADDR_OFFSET);
	int cookie = cmdq_thread_get_cookie(thread);

	dev_err(dev, "IRQ: error thread=0x%x\n",
		(u32)(thread->base - cmdq->base));
	dev_err(dev, "IRQ: Thread PC: 0x%08x, End PC:0x%08x\n",
		curr_pa, end_pa);

	/*
	 * we assume error happens BEFORE EOC
	 * because it wouldn't be error if this interrupt is issue by EOC.
	 * so we should inc by 1 to locate "current" task
	 */
	cookie++;

	/* set the issued task to error state */
	if (thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD]) {
		struct cmdq_task *task;
		struct cmdq_cb_data cmdq_cb_data;

		task = thread->cur_task[cookie % CMDQ_MAX_TASK_IN_THREAD];
		/* still call cb to prevent lock */
		if (task->cb.cb) {
			cmdq_cb_data.err = true;
			cmdq_cb_data.data = task->cb.data;
			task->cb.cb(cmdq_cb_data);
		}
		cmdq_thread_remove_task_by_index(
				thread, cookie % CMDQ_MAX_TASK_IN_THREAD,
				TASK_STATE_ERROR);
	} else {
		dev_err(dev,
			"IRQ: can not find task in %s, pc:0x%08x, end_pc:0x%08x\n",
			__func__, curr_pa, end_pa);
		if (thread->task_count <= 0) {
			/*
			 * suspend HW thread first,
			 * so that we work in a consistent state
			 * outer function should acquire spinlock:
			 *   cmdq->exec_lock
			 */
			if (cmdq_thread_suspend(cmdq, thread) < 0)
				dev_err(dev, "IRQ: suspend HW thread failed!");

			cmdq_thread_disable(cmdq, thread);
			dev_err(dev,
				"IRQ: there is no task for thread (0x%x)\n",
				(u32)(thread->base - cmdq->base));
		}
	}

	/* set the remain tasks to done state */
	cmdq_handle_done_internal(cmdq, thread, cookie);
}

static void cmdq_handle_done(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	cmdq_handle_done_internal(cmdq, thread,
				  cmdq_thread_get_cookie(thread));
}

static void cmdq_handle_irq(struct cmdq *cmdq, int tid)
{
	struct device *dev = cmdq->dev;
	struct cmdq_thread *thread = &cmdq->thread[tid];
	unsigned long flags = 0L;
	int value;
	int enabled;

	/*
	 * normal execution, marks tasks done and remove from thread
	 * also, handle "loop CB fail" case
	 */
	spin_lock_irqsave(&cmdq->exec_lock, flags);

	/*
	 * it is possible for another CPU core
	 * to run "release task" right before we acquire the spin lock
	 * and thus reset / disable this HW thread
	 * so we check both the IRQ flag and the enable bit of this thread
	 */
	value = cmdq_thread_readl(thread, CMDQ_THR_IRQ_STATUS_OFFSET);
	if (!(value & CMDQ_THR_IRQ_MASK)) {
		dev_err(dev,
			"IRQ: thread 0x%x got interrupt but IRQ flag=0x%x\n",
			(u32)(thread->base - cmdq->base), value);
		spin_unlock_irqrestore(&cmdq->exec_lock, flags);
		return;
	}

	enabled = cmdq_thread_readl(thread, CMDQ_THR_ENABLE_TASK_OFFSET);
	if (!(enabled & CMDQ_THR_ENABLED)) {
		dev_err(dev,
			"IRQ: thread 0x%x got interrupt but enabled=0x%x\n",
			(u32)(thread->base - cmdq->base), enabled);
		spin_unlock_irqrestore(&cmdq->exec_lock, flags);
		return;
	}

	/*
	 * Move the reset IRQ before read HW cookie
	 * to prevent race condition and save the cost of suspend
	 */
	cmdq_thread_writel(thread, ~value, CMDQ_THR_IRQ_STATUS_OFFSET);

	if (value & CMDQ_THR_IRQ_ERROR)
		cmdq_handle_error(cmdq, thread);
	else if (value & CMDQ_THR_IRQ_DONE)
		cmdq_handle_done(cmdq, thread);

	spin_unlock_irqrestore(&cmdq->exec_lock, flags);
}

static int cmdq_resumed_notifier(struct cmdq *cmdq)
{
	unsigned long flags = 0L;

	spin_lock_irqsave(&cmdq->thread_lock, flags);
	cmdq->suspended = false;

	/*
	 * during suspended, there may be queued tasks.
	 * we should process them if any.
	 */
	queue_work(cmdq->task_consume_wq,
		   &cmdq->task_consume_wait_queue_item);

	spin_unlock_irqrestore(&cmdq->thread_lock, flags);

	return 0;
}

static void cmdq_consume_waiting_list(struct work_struct *work)
{
	struct cmdq *cmdq = container_of(work, struct cmdq,
					 task_consume_wait_queue_item);
	struct device *dev = cmdq->dev;
	struct cmdq_task *task, *tmp;

	/*
	 * when we're suspended,
	 * do not execute any tasks. delay & hold them.
	 */
	if (cmdq->suspended)
		return;

	mutex_lock(&cmdq->task_mutex);

	if (list_empty(&cmdq->task_wait_list)) {
		mutex_unlock(&cmdq->task_mutex);
		return;
	}

	list_for_each_entry_safe(task, tmp, &cmdq->task_wait_list, list_entry) {
		struct cmdq_thread *thread = NULL;
		int err;

		thread = cmdq_thread_get(cmdq, task->engine_flag);
		if (!thread) {
			dev_warn(dev, "acquire thread fail. wait\n");
			continue;
		}

		/* start execution */
		list_move_tail(&task->list_entry,
			       &cmdq->task_active_list);
		err = cmdq_task_exec_async(task, thread);
		if (err < 0) {
			dev_err(dev, "start task(0x%p) fail\n", task);
			/*
			 * mask as error and release in
			 * cmdq_task_wait_and_release()
			 */
			task->task_state = TASK_STATE_START_ERROR;
		}
	}

	/*
	 * Wake up waiting task(s) whether success or not
	 * because wake up condition will check task's thread.
	 * (cmdq_task_wait_and_release)
	 */
	wake_up_all(&cmdq->thread_dispatch_queue);

	mutex_unlock(&cmdq->task_mutex);
}

static int cmdq_task_submit_async(struct cmdq_command *command,
				  struct cmdq_task **task_out,
				  struct cmdq_task_cb *cb)
{
	struct cmdq *cmdq = command->cmdq;

	/* creates a new task and put into tail of waiting list */
	*task_out = cmdq_task_acquire(command, cb);

	if (!(*task_out))
		return -EFAULT;

	/*
	 * Consume the waiting list.
	 * This may or may not execute the task, depending on available threads.
	 */
	cmdq_consume_waiting_list(&cmdq->task_consume_wait_queue_item);

	return 0;
}

static int cmdq_task_handle_error_result(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	struct cmdq_thread *thread = task->thread;
	int err, i, cookie;
	struct cmdq_task *next_task;
	unsigned long thread_pc;
	u32 *prev_va, *curr_va;
	u32 irq_flag;

	dev_err(dev,
		"task(0x%p) state is not TASK_STATE_DONE, but %d.\n",
		task, task->task_state);

	/*
	 * suspend HW thread first,
	 * so that we work in a consistent state
	 */
	err = cmdq_thread_suspend(cmdq, thread);
	if (err < 0)
		return err;

	/* The cookie of the task currently being processed */
	cookie = cmdq_thread_get_cookie(thread) + 1;
	thread_pc = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);

	/* process any pending IRQ */
	irq_flag = cmdq_thread_readl(thread, CMDQ_THR_IRQ_STATUS_OFFSET);
	if (irq_flag & CMDQ_THR_IRQ_ERROR)
		cmdq_handle_error(cmdq, thread);
	else if (irq_flag & CMDQ_THR_IRQ_DONE)
		cmdq_handle_done(cmdq, thread);
	cmdq_thread_writel(thread, ~irq_flag, CMDQ_THR_IRQ_STATUS_OFFSET);

	if (task->task_state == TASK_STATE_DONE)
		return 0; /* success after handling pending irq */

	dev_err(dev, "task 0x%p timeout or killed\n", task);

	if (task->task_state == TASK_STATE_BUSY) {
		/* Task is running, so we force to remove it. */
		for (i = 0; i < ARRAY_SIZE(thread->cur_task); i++) {
			if (thread->cur_task[i] == task) {
				cmdq_thread_remove_task_by_index(
						thread, i, TASK_STATE_ERROR);
				break;
			}
		}
	}

	/* find task's jump destination or no next task*/
	next_task = NULL;
	curr_va = task->va_base;
	if (curr_va[task->num_cmd - 1] == CMDQ_JUMP_BY_PA)
		next_task = cmdq_thread_search_task_by_pc(
				thread, curr_va[task->num_cmd - 2]);

	/* remove task from the chain of thread->cur_task. */
	if (task->num_cmd && thread_pc >= task->mva_base &&
	    thread_pc < (task->mva_base + task->command_size)) {
		if (next_task) {
			/* cookie already +1 */
			cmdq_thread_writel(thread, cookie,
					   CMDQ_THR_EXEC_CNT_OFFSET);
			thread->wait_cookie = cookie + 1;
			cmdq_thread_writel(thread, next_task->mva_base,
					   CMDQ_THR_CURR_ADDR_OFFSET);
		}
	} else {
		for (i = 0; i < CMDQ_MAX_TASK_IN_THREAD; i++) {
			struct cmdq_task *prev_task;
			u32 prev_num, curr_num;

			prev_task = thread->cur_task[i];
			if (!prev_task)
				continue;

			prev_va = prev_task->va_base;
			prev_num = prev_task->num_cmd;

			curr_va = task->va_base;
			curr_num = task->num_cmd;

			/* find which task JUMP into task */
			if (prev_va[prev_num - 2] == task->mva_base &&
			    prev_va[prev_num - 1] == CMDQ_JUMP_BY_PA) {
				/* Copy Jump instruction */
				prev_va[prev_num - 2] = curr_va[curr_num - 2];
				prev_va[prev_num - 1] = curr_va[curr_num - 1];

				if (next_task)
					cmdq_thread_reorder_task_array(
							thread, i);

				/* re-fetch command */
				cmdq_thread_invalidate_fetched_data(thread);

				break;
			}
		}
	}

	return -ECANCELED;
}

static int cmdq_task_wait_result(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct cmdq_thread *thread = task->thread;
	int err = 0;
	unsigned long flags;

	/*
	 * Note that although we disable IRQ, HW continues to execute
	 * so it's possible to have pending IRQ
	 */
	spin_lock_irqsave(&cmdq->exec_lock, flags);

	/* suspend, so just return */
	if (atomic_read(&cmdq->suspending) &&
	    task->task_state == TASK_STATE_KILLED) {
		spin_unlock_irqrestore(&cmdq->exec_lock, flags);
		return 0;
	}

	if (task->task_state != TASK_STATE_DONE)
		err = cmdq_task_handle_error_result(task);

	if (thread->task_count <= 0)
		cmdq_thread_disable(cmdq, thread);
	else
		cmdq_thread_resume(thread);

	spin_unlock_irqrestore(&cmdq->exec_lock, flags);

	return err;
}

static int cmdq_task_wait_done(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	int wait_q;
	unsigned long timeout = msecs_to_jiffies(
			CMDQ_ACQUIRE_THREAD_TIMEOUT_MS);

	/* wait for acquiring thread (cmdq_consume_waiting_list) */
	wait_q = wait_event_timeout(
			cmdq->thread_dispatch_queue,
			task->thread, timeout);

	if (task->task_state == TASK_STATE_START_ERROR)
		return -EINVAL;

	if (!wait_q) {
		mutex_lock(&cmdq->task_mutex);

		/*
		 * It's possible that the task was just consumed,
		 * so check again.
		 */
		if (!task->thread) {
			/*
			 * Task may have released,
			 * or starved to death.
			 */
			dev_err(dev,
				"task(0x%p) timeout with invalid thread\n",
				task);

			/*
			 * remove from waiting list,
			 * so that it won't be consumed in the future
			 */
			list_del_init(&task->list_entry);

			mutex_unlock(&cmdq->task_mutex);
			return -EINVAL;
		}

		/* valid thread, so we keep going */
		mutex_unlock(&cmdq->task_mutex);
	}

	/* start to wait */
	wait_q = wait_event_timeout(task->thread->wait_queue,
				    (task->task_state != TASK_STATE_BUSY &&
				     task->task_state != TASK_STATE_WAITING),
				    msecs_to_jiffies(CMDQ_DEFAULT_TIMEOUT_MS));
	if (!wait_q)
		dev_dbg(dev, "timeout!\n");

	/* wake up and continue */
	return cmdq_task_wait_result(task);
}

static int cmdq_task_wait_and_release(struct cmdq_task *task)
{
	int err = cmdq_task_wait_done(task);

	/* release regardless of success or not */
	cmdq_thread_put(task->cmdq, task->thread);
	if (!atomic_read(&task->cmdq->suspending))
		cmdq_task_release_internal(task);

	return err;
}

static void cmdq_auto_release(struct work_struct *work_item)
{
	struct cmdq_task *task = container_of(work_item, struct cmdq_task,
					      auto_release_work);
	struct cmdq *cmdq = task->cmdq;
	struct cmdq_task_cb cb = task->cb;
	int err = cmdq_task_wait_and_release(task);
	struct cmdq_cb_data cmdq_cb_data;

	/* isr fail, so call cb here to prevent lock */
	if (err && cb.cb) {
		cmdq_cb_data.err = true;
		cmdq_cb_data.data = cb.data;
		cb.cb(cmdq_cb_data);
	}

	/* prevent no more flush or interrupt to consume waiting tasks */
	if (err)
		queue_work(cmdq->task_consume_wq,
			   &cmdq->task_consume_wait_queue_item);
}

static void cmdq_task_auto_release(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;

	/*
	 * the work item is embeded in task already
	 * but we need to initialized it
	 */
	INIT_WORK(&task->auto_release_work, cmdq_auto_release);
	queue_work(cmdq->task_auto_release_wq, &task->auto_release_work);
}

static int cmdq_task_submit(struct cmdq_command *command)
{
	struct device *dev = command->cmdq->dev;
	int err;
	struct cmdq_task *task;

	err = cmdq_task_submit_async(command, &task, NULL);
	if (err < 0) {
		dev_err(dev, "cmdq_task_submit_async failed=%d\n", err);
		return err;
	}

	err = cmdq_task_wait_and_release(task);
	if (err < 0)
		dev_err(dev, "task(0x%p) wait fail\n", task);

	return err;
}

static void cmdq_deinitialize(struct platform_device *pdev)
{
	struct cmdq *cmdq = platform_get_drvdata(pdev);
	int i;
	struct list_head *lists[] = {
		&cmdq->task_active_list,
		&cmdq->task_wait_list
	};

	/*
	 * Directly destroy the auto release WQ
	 * since we're going to release tasks anyway.
	 */
	destroy_workqueue(cmdq->task_auto_release_wq);
	cmdq->task_auto_release_wq = NULL;

	destroy_workqueue(cmdq->task_consume_wq);
	cmdq->task_consume_wq = NULL;

	/* release all tasks in both list */
	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		struct cmdq_task *task, *tmp;

		list_for_each_entry_safe(task, tmp, lists[i], list_entry) {
			cmdq_task_free_command_buffer(task);
			list_del(&task->list_entry);
			kmem_cache_free(cmdq->task_cache, task);
		}
	}

	kmem_cache_destroy(cmdq->task_cache);
	cmdq->task_cache = NULL;
}

static irqreturn_t cmdq_irq_handler(int irq, void *dev)
{
	struct cmdq *cmdq = dev;
	u32 irq_status;
	int i;

	irq_status = readl(cmdq->base + CMDQ_CURR_IRQ_STATUS_OFFSET);
	irq_status &= CMDQ_IRQ_MASK;
	irq_status ^= CMDQ_IRQ_MASK;

	if (!irq_status)
		return IRQ_NONE;

	while (irq_status) {
		i = ffs(irq_status) - 1;
		irq_status &= ~BIT(i);
		cmdq_handle_irq(cmdq, i);
	}

	queue_work(cmdq->task_consume_wq, &cmdq->task_consume_wait_queue_item);
	return IRQ_HANDLED;
}

static int cmdq_initialize(struct cmdq *cmdq)
{
	int i;

	/* initial mutex, spinlock */
	mutex_init(&cmdq->task_mutex);
	spin_lock_init(&cmdq->thread_lock);
	spin_lock_init(&cmdq->exec_lock);

	/* initial wait queue for thread acquiring */
	init_waitqueue_head(&cmdq->thread_dispatch_queue);

	/* create task pool */
	cmdq->task_cache = kmem_cache_create(
			CMDQ_DRIVER_DEVICE_NAME "_task",
			sizeof(struct cmdq_task),
			__alignof__(struct cmdq_task),
			SLAB_POISON | SLAB_HWCACHE_ALIGN | SLAB_RED_ZONE,
			&cmdq_task_ctor);

	/* initialize task lists */
	INIT_LIST_HEAD(&cmdq->task_active_list);
	INIT_LIST_HEAD(&cmdq->task_wait_list);
	INIT_WORK(&cmdq->task_consume_wait_queue_item,
		  cmdq_consume_waiting_list);

	cmdq->task_auto_release_wq = alloc_ordered_workqueue(
			"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "cmdq_auto_release");
	cmdq->task_consume_wq = alloc_ordered_workqueue(
			"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "cmdq_task");

	/* initialize cmdq thread */
	for (i = 0; i < ARRAY_SIZE(cmdq->thread) ; i++) {
		cmdq->thread[i].base = cmdq->base + CMDQ_THR_BASE +
				CMDQ_THR_SHIFT * i;
		init_waitqueue_head(&cmdq->thread[i].wait_queue);
	}

	return 0;
}

static int cmdq_rec_realloc_cmd_buffer(struct cmdq_rec *rec, size_t size)
{
	void *new_buf;

	new_buf = krealloc(rec->buf, size, GFP_KERNEL | __GFP_ZERO);
	if (!new_buf)
		return -ENOMEM;
	rec->buf = new_buf;
	rec->buf_size = size;
	return 0;
}

struct cmdq_base *cmdq_register_device(struct device *dev)
{
	struct cmdq_base *cmdq_base;
	struct resource res;
	int subsys;
	u32 base;

	if (of_address_to_resource(dev->of_node, 0, &res))
		return NULL;
	base = (u32)res.start;

	subsys = cmdq_subsys_base_addr_to_id(base >> 16);
	if (subsys < 0)
		return NULL;

	cmdq_base = devm_kmalloc(dev, sizeof(*cmdq_base), GFP_KERNEL);
	if (!cmdq_base)
		return NULL;
	cmdq_base->subsys = subsys;
	cmdq_base->base = base;

	return cmdq_base;
}
EXPORT_SYMBOL(cmdq_register_device);

int cmdq_rec_create(struct device *dev, u64 engine_flag,
		    struct cmdq_rec **rec_ptr)
{
	struct cmdq_rec *rec;
	int err;

	rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		return -ENOMEM;

	rec->cmdq = dev_get_drvdata(dev);
	rec->engine_flag = engine_flag;

	err = cmdq_rec_realloc_cmd_buffer(rec, CMDQ_INITIAL_CMD_BLOCK_SIZE);
	if (err) {
		kfree(rec);
		return err;
	}

	*rec_ptr = rec;

	return 0;
}
EXPORT_SYMBOL(cmdq_rec_create);

static int cmdq_rec_append_command(struct cmdq_rec *rec, enum cmdq_code code,
				   u32 arg_a, u32 arg_b)
{
	u32 *cmd_ptr;
	int err;

	if (WARN_ON(rec->finalized))
		return -EBUSY;

	/* check if we have sufficient buffer size */
	if (unlikely(rec->command_size + CMDQ_INST_SIZE > rec->buf_size)) {
		err = cmdq_rec_realloc_cmd_buffer(rec, rec->buf_size * 2);
		if (err)
			return err;
	}

	cmd_ptr = (u32 *)(rec->buf + rec->command_size);

	switch (code) {
	case CMDQ_CODE_MOVE:
		cmd_ptr[0] = arg_b;
		cmd_ptr[1] = CMDQ_CODE_MOVE << CMDQ_OP_CODE_SHIFT;
		break;
	case CMDQ_CODE_JUMP:
		cmd_ptr[0] = arg_b;
		cmd_ptr[1] = CMDQ_CODE_JUMP << CMDQ_OP_CODE_SHIFT;
		break;
	case CMDQ_CODE_WFE:
		/*
		 * bit 0-11: wait_value, 1
		 * bit 15: to_wait, true
		 * bit 16-27: update_value, 0
		 * bit 31: to_update, true
		 */
		cmd_ptr[0] = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT |
			     CMDQ_WFE_WAIT_VALUE;
		cmd_ptr[1] = (CMDQ_CODE_WFE << CMDQ_OP_CODE_SHIFT) | arg_a;
		break;
	case CMDQ_CODE_CLEAR_EVENT:
		/*
		 * bit 0-11: wait_value, 0
		 * bit 15: to_wait, false
		 * bit 16-27: update_value, 0
		 * bit 31: to_update, true
		 */
		cmd_ptr[0] = CMDQ_WFE_UPDATE;
		cmd_ptr[1] = (CMDQ_CODE_WFE << CMDQ_OP_CODE_SHIFT) | arg_a;
		break;
	case CMDQ_CODE_EOC:
		cmd_ptr[0] = arg_b;
		cmd_ptr[1] = CMDQ_CODE_EOC << CMDQ_OP_CODE_SHIFT;
		break;
	default:
		return -EFAULT;
	}

	rec->command_size += CMDQ_INST_SIZE;
	return 0;
}

static int cmdq_rec_append_write(struct cmdq_rec *rec, u32 subsys, u32 addr,
				 u32 val)
{
	u32 *cmd_ptr;
	int err;

	if (WARN_ON(rec->finalized))
		return -EBUSY;

	/* check if we have sufficient buffer size */
	if (unlikely(rec->command_size + CMDQ_INST_SIZE > rec->buf_size)) {
		err = cmdq_rec_realloc_cmd_buffer(rec, rec->buf_size * 2);
		if (err)
			return err;
	}

	cmd_ptr = (u32 *)(rec->buf + rec->command_size);

	cmd_ptr[0] = val;
	cmd_ptr[1] = (CMDQ_CODE_WRITE << CMDQ_OP_CODE_SHIFT) |
		     (addr & CMDQ_ARG_A_WRITE_MASK) |
		     ((subsys & CMDQ_SUBSYS_MASK) << CMDQ_SUBSYS_SHIFT);

	rec->command_size += CMDQ_INST_SIZE;
	return 0;
}

int cmdq_rec_write(struct cmdq_rec *rec, u32 value, struct cmdq_base *base,
		   u32 offset)
{
	return cmdq_rec_append_write(rec, base->subsys,
				     base->base + offset, value);
}
EXPORT_SYMBOL(cmdq_rec_write);

int cmdq_rec_write_mask(struct cmdq_rec *rec, u32 value,
			struct cmdq_base *base, u32 offset, u32 mask)
{
	int err;
	u32 addr = base->base + offset;

	if (mask != 0xffffffff) {
		err = cmdq_rec_append_command(rec, CMDQ_CODE_MOVE, 0, ~mask);
		if (err)
			return err;

		addr = addr | CMDQ_ENABLE_MASK;
	}

	return cmdq_rec_append_write(rec, base->subsys, addr, value);
}
EXPORT_SYMBOL(cmdq_rec_write_mask);

int cmdq_rec_wfe(struct cmdq_rec *rec, enum cmdq_event event)
{
	if (event >= CMDQ_MAX_HW_EVENT_COUNT || event < 0)
		return -EINVAL;

	return cmdq_rec_append_command(rec, CMDQ_CODE_WFE, event, 0);
}
EXPORT_SYMBOL(cmdq_rec_wfe);

int cmdq_rec_clear_event(struct cmdq_rec *rec, enum cmdq_event event)
{
	if (event >= CMDQ_MAX_HW_EVENT_COUNT || event < 0)
		return -EINVAL;

	return cmdq_rec_append_command(rec, CMDQ_CODE_CLEAR_EVENT, event, 0);
}
EXPORT_SYMBOL(cmdq_rec_clear_event);

static int cmdq_rec_fill_command(struct cmdq_rec *rec,
				 struct cmdq_command *command)
{
	if (!rec->finalized) {
		int err;

		/* insert EOC and generate IRQ for each command iteration */
		err = cmdq_rec_append_command(rec, CMDQ_CODE_EOC, 0,
					      CMDQ_EOC_IRQ_EN);
		if (err)
			return err;

		/* JUMP to begin */
		err = cmdq_rec_append_command(rec, CMDQ_CODE_JUMP, 0,
					      CMDQ_INST_SIZE);
		if (err)
			return err;

		rec->finalized = true;
	}

	command->cmdq = rec->cmdq;
	command->engine_flag = rec->engine_flag;
	command->base = rec->buf;
	command->size = rec->command_size;
	return 0;
}

int cmdq_rec_flush(struct cmdq_rec *rec)
{
	int err;
	struct cmdq_command command;

	err = cmdq_rec_fill_command(rec, &command);
	if (err)
		return err;

	return cmdq_task_submit(&command);
}
EXPORT_SYMBOL(cmdq_rec_flush);

int cmdq_rec_flush_async(struct cmdq_rec *rec, cmdq_async_flush_cb cb,
			 void *data)
{
	int err;
	struct cmdq_command command;
	struct cmdq_task *task;
	struct cmdq_task_cb task_cb;

	err = cmdq_rec_fill_command(rec, &command);
	if (err)
		return err;

	task_cb.cb = cb;
	task_cb.data = data;

	err = cmdq_task_submit_async(&command, &task, &task_cb);
	if (err)
		return err;

	cmdq_task_auto_release(task);

	return 0;
}
EXPORT_SYMBOL(cmdq_rec_flush_async);

void cmdq_rec_destroy(struct cmdq_rec *rec)
{
	kfree(rec->buf);
	kfree(rec);
}
EXPORT_SYMBOL(cmdq_rec_destroy);

static int cmdq_pm_notifier_cb(struct notifier_block *nb, unsigned long event,
			       void *ptr)
{
	struct cmdq *cmdq = container_of(nb, struct cmdq, pm_notifier);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		/*
		 * Going to suspend the system
		 * The next stage is freeze process.
		 * We will queue all request in suspend callback,
		 * so don't care this stage
		 */
		return NOTIFY_DONE;
	case PM_POST_SUSPEND:
		/*
		 * processes had resumed in previous stage
		 * (system resume callback)
		 * resume CMDQ driver to execute.
		 */
		cmdq_resumed_notifier(cmdq);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_DONE;
}

static int cmdq_suspend(struct device *dev)
{
	struct cmdq *cmdq = dev_get_drvdata(dev);
	u32 exec_threads = readl(cmdq->base + CMDQ_CURR_LOADED_THR_OFFSET);
	int ref_count;
	unsigned long flags;

	/*
	 * lock to prevent cmdq_core_consume_waiting_list() and
	 * cmdq_core_acquire_task(), i.e. no new active tasks
	 */
	mutex_lock(&cmdq->task_mutex);

	ref_count = atomic_read(&cmdq->thread_usage);
	atomic_set(&cmdq->suspending, 1);

	if (ref_count > 0 || exec_threads & CMDQ_THR_EXECUTING) {
		struct cmdq_task *task, *tmp;
		int i;

		dev_err(dev, "suspend: other running, kill tasks.\n");
		dev_err(dev, "threads:0x%08x, ref:%d, AL empty:%d, base:0x%p\n",
			exec_threads, ref_count,
			list_empty(&cmdq->task_active_list), cmdq->base);

		/*
		 * We need to ensure the system is ready to suspend,
		 * so kill all running CMDQ tasks and release HW engines.
		 */

		/* remove all active task from thread */
		list_for_each_entry_safe(task, tmp, &cmdq->task_active_list,
					 list_entry) {
			bool already_done = false;

			if (!task->thread)
				continue;

			spin_lock_irqsave(&cmdq->exec_lock, flags);
			if (task->task_state == TASK_STATE_BUSY) {
				/* still wait_event */
				cmdq_thread_force_remove_task(task);
				task->task_state = TASK_STATE_KILLED;
			} else {
				/* almost finish its work */
				already_done = true;
			}
			spin_unlock_irqrestore(&cmdq->exec_lock, flags);

			/*
			 * TASK_STATE_KILLED will unlock
			 * wait_event_timeout in cmdq_task_wait_done(),
			 * so flush_work to wait auto release flow.
			 *
			 * We don't know processes running order,
			 * so call cmdq_task_release_unlocked() here to
			 * prevent releasing task before flush_work, and
			 * also to prevent deadlock of task_mutex.
			 */
			if (!already_done) {
				flush_work(&task->auto_release_work);
				cmdq_task_release_unlocked(task);
			}
		}
		dev_err(dev, "suspend: AL empty:%d\n",
			list_empty(&cmdq->task_active_list));

		/* disable all HW thread */
		dev_err(dev, "suspend: disable all HW threads\n");
		for (i = 0; i < CMDQ_MAX_THREAD_COUNT; i++)
			cmdq_thread_disable(cmdq, &cmdq->thread[i]);

		/* reset all cmdq_thread */
		memset(&cmdq->thread[0], 0, sizeof(cmdq->thread));
	}

	spin_lock_irqsave(&cmdq->thread_lock, flags);
	cmdq->suspended = true;
	spin_unlock_irqrestore(&cmdq->thread_lock, flags);
	atomic_set(&cmdq->suspending, 0);

	mutex_unlock(&cmdq->task_mutex);

	/* ALWAYS allow suspend */
	return 0;
}

static int cmdq_resume(struct device *dev)
{
	return 0;
}

static int cmdq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *res;
	struct cmdq *cmdq;
	int err;

	cmdq = devm_kzalloc(dev, sizeof(*cmdq), GFP_KERNEL);
	if (!cmdq)
		return -ENOMEM;
	cmdq->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cmdq->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(cmdq->base)) {
		dev_err(dev, "failed to ioremap gce\n");
		return PTR_ERR(cmdq->base);
	}

	cmdq->irq = irq_of_parse_and_map(node, 0);
	if (!cmdq->irq) {
		dev_err(dev, "failed to get irq\n");
		return -EINVAL;
	}

	dev_dbg(dev, "cmdq device: addr:0x%p, va:0x%p, irq:%d\n",
		dev, cmdq->base, cmdq->irq);

	/* init cmdq and save to device private data */
	err = cmdq_initialize(cmdq);
	if (err) {
		dev_err(dev, "failed to init cmdq\n");
		return err;
	}
	platform_set_drvdata(pdev, cmdq);

	err = devm_request_irq(dev, cmdq->irq, cmdq_irq_handler, IRQF_SHARED,
			       CMDQ_DRIVER_DEVICE_NAME, cmdq);
	if (err) {
		dev_err(dev, "failed to register ISR (%d)\n", err);
		goto fail;
	}

	cmdq->clock = devm_clk_get(dev, CMDQ_CLK_NAME);
	if (IS_ERR(cmdq->clock)) {
		dev_err(dev, "failed to get clk:%s\n", CMDQ_CLK_NAME);
		err = PTR_ERR(cmdq->clock);
		goto fail;
	}

	/* hibernation and suspend events */
	cmdq->pm_notifier.notifier_call = cmdq_pm_notifier_cb;
	cmdq->pm_notifier.priority = 5;
	err = register_pm_notifier(&cmdq->pm_notifier);
	if (err) {
		dev_err(dev, "failed to register cmdq pm notifier\n");
		goto fail;
	}

	return 0;

fail:
	cmdq_deinitialize(pdev);
	return err;
}

static int cmdq_remove(struct platform_device *pdev)
{
	struct cmdq *cmdq = platform_get_drvdata(pdev);
	int status;

	status = unregister_pm_notifier(&cmdq->pm_notifier);
	if (status)
		dev_err(&pdev->dev, "unregister pm notifier failed\n");

	cmdq_deinitialize(pdev);
	return 0;
}

static const struct dev_pm_ops cmdq_pm_ops = {
	.suspend = cmdq_suspend,
	.resume = cmdq_resume,
};

static const struct of_device_id cmdq_of_ids[] = {
	{.compatible = "mediatek,mt8173-gce",},
	{}
};

static struct platform_driver cmdq_drv = {
	.probe = cmdq_probe,
	.remove = cmdq_remove,
	.driver = {
		.name = CMDQ_DRIVER_DEVICE_NAME,
		.owner = THIS_MODULE,
		.pm = &cmdq_pm_ops,
		.of_match_table = cmdq_of_ids,
	}
};

builtin_platform_driver(cmdq_drv);
