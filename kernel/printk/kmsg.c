#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/kmsg_dump.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/stat.h>
#include <linux/syslog.h>
#include <linux/uio.h>
#include <linux/wait.h>

#include <asm/uaccess.h>

#include "printk.h"

/* /dev/kmsg - userspace message inject/listen interface */
struct devkmsg_user {
	u64 seq;
	u32 idx;
	enum log_flags prev;
	struct mutex lock;
	char buf[CONSOLE_EXT_LOG_MAX];
};

static ssize_t devkmsg_write(struct kiocb *iocb, struct iov_iter *from)
{
	char *buf, *line;
	int level = default_message_loglevel;
	int facility = 1;	/* LOG_USER */
	size_t len = iov_iter_count(from);
	ssize_t ret = len;

	if (len > LOG_LINE_MAX)
		return -EINVAL;
	buf = kmalloc(len+1, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[len] = '\0';
	if (copy_from_iter(buf, len, from) != len) {
		kfree(buf);
		return -EFAULT;
	}

	/*
	 * Extract and skip the syslog prefix <[0-9]*>. Coming from userspace
	 * the decimal value represents 32bit, the lower 3 bit are the log
	 * level, the rest are the log facility.
	 *
	 * If no prefix or no userspace facility is specified, we
	 * enforce LOG_USER, to be able to reliably distinguish
	 * kernel-generated messages from userspace-injected ones.
	 */
	line = buf;
	if (line[0] == '<') {
		char *endp = NULL;
                unsigned int u;

		u = simple_strtoul(line + 1, &endp, 10);
		if (endp && endp[0] == '>') {
			level = LOG_LEVEL(u);
			if (LOG_FACILITY(u) != 0)
				facility = LOG_FACILITY(u);
			endp++;
			len -= endp - line;
			line = endp;
		}
	}

	printk_emit(facility, level, NULL, 0, "%s", line);
	kfree(buf);
	return ret;
}

static ssize_t devkmsg_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct devkmsg_user *user = file->private_data;
	struct printk_log *msg;
	size_t len;
	ssize_t ret;

	if (!user)
		return -EBADF;

	ret = mutex_lock_interruptible(&user->lock);
	if (ret)
		return ret;
	raw_spin_lock_irq(&logbuf_lock);
	while (user->seq == log_next_seq) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			raw_spin_unlock_irq(&logbuf_lock);
			goto out;
		}

		raw_spin_unlock_irq(&logbuf_lock);
		ret = wait_event_interruptible(log_wait,
					       user->seq != log_next_seq);
		if (ret)
			goto out;
		raw_spin_lock_irq(&logbuf_lock);
	}

	if (user->seq < log_first_seq) {
		/* our last seen message is gone, return error and reset */
		user->idx = log_first_idx;
		user->seq = log_first_seq;
		ret = -EPIPE;
		raw_spin_unlock_irq(&logbuf_lock);
		goto out;
	}

	msg = log_from_idx(user->idx);
	len = msg_print_ext_header(user->buf, sizeof(user->buf),
				   msg, user->seq, user->prev);
	len += msg_print_ext_body(user->buf + len, sizeof(user->buf) - len,
				  log_dict(msg), msg->dict_len,
				  log_text(msg), msg->text_len);

	user->prev = msg->flags;
	user->idx = log_next(user->idx);
	user->seq++;
	raw_spin_unlock_irq(&logbuf_lock);

	if (len > count) {
		ret = -EINVAL;
		goto out;
	}

	if (copy_to_user(buf, user->buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	ret = len;
out:
	mutex_unlock(&user->lock);
	return ret;
}

static loff_t devkmsg_llseek(struct file *file, loff_t offset, int whence)
{
	struct devkmsg_user *user = file->private_data;
	loff_t ret = 0;

	if (!user)
		return -EBADF;
	if (offset)
		return -ESPIPE;

	raw_spin_lock_irq(&logbuf_lock);
	switch (whence) {
	case SEEK_SET:
		/* the first record */
		user->idx = log_first_idx;
		user->seq = log_first_seq;
		break;
	case SEEK_DATA:
		/*
		 * The first record after the last SYSLOG_ACTION_CLEAR,
		 * like issued by 'dmesg -c'. Reading /dev/kmsg itself
		 * changes no global state, and does not clear anything.
		 */
		user->idx = clear_idx;
		user->seq = clear_seq;
		break;
	case SEEK_END:
		/* after the last record */
		user->idx = log_next_idx;
		user->seq = log_next_seq;
		break;
	default:
		ret = -EINVAL;
	}
	raw_spin_unlock_irq(&logbuf_lock);
	return ret;
}

static unsigned int devkmsg_poll(struct file *file, poll_table *wait)
{
	struct devkmsg_user *user = file->private_data;
	int ret = 0;

	if (!user)
		return POLLERR|POLLNVAL;

	poll_wait(file, &log_wait, wait);

	raw_spin_lock_irq(&logbuf_lock);
	if (user->seq < log_next_seq) {
		/* return error when data has vanished underneath us */
		if (user->seq < log_first_seq)
			ret = POLLIN|POLLRDNORM|POLLERR|POLLPRI;
		else
			ret = POLLIN|POLLRDNORM;
	}
	raw_spin_unlock_irq(&logbuf_lock);

	return ret;
}

static int devkmsg_open(struct inode *inode, struct file *file)
{
	struct devkmsg_user *user;
	int err;

	/* write-only does not need any file context */
	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return 0;

	err = check_syslog_permissions(SYSLOG_ACTION_READ_ALL,
				       SYSLOG_FROM_READER);
	if (err)
		return err;

	user = kmalloc(sizeof(struct devkmsg_user), GFP_KERNEL);
	if (!user)
		return -ENOMEM;

	mutex_init(&user->lock);

	raw_spin_lock_irq(&logbuf_lock);
	user->idx = log_first_idx;
	user->seq = log_first_seq;
	raw_spin_unlock_irq(&logbuf_lock);

	file->private_data = user;
	return 0;
}

static int devkmsg_release(struct inode *inode, struct file *file)
{
	struct devkmsg_user *user = file->private_data;

	if (!user)
		return 0;

	mutex_destroy(&user->lock);
	kfree(user);
	return 0;
}

const struct file_operations kmsg_fops = {
	.open = devkmsg_open,
	.read = devkmsg_read,
	.write_iter = devkmsg_write,
	.llseek = devkmsg_llseek,
	.poll = devkmsg_poll,
	.release = devkmsg_release,
};

static DEFINE_SPINLOCK(dump_list_lock);
static LIST_HEAD(dump_list);

/**
 * kmsg_dump_register - register a kernel log dumper.
 * @dumper: pointer to the kmsg_dumper structure
 *
 * Adds a kernel log dumper to the system. The dump callback in the
 * structure will be called when the kernel oopses or panics and must be
 * set. Returns zero on success and %-EINVAL or %-EBUSY otherwise.
 */
int kmsg_dump_register(struct kmsg_dumper *dumper)
{
	unsigned long flags;
	int err = -EBUSY;

	/* The dump callback needs to be set */
	if (!dumper->dump)
		return -EINVAL;

	spin_lock_irqsave(&dump_list_lock, flags);
	/* Don't allow registering multiple times */
	if (!dumper->registered) {
		dumper->registered = 1;
		list_add_tail_rcu(&dumper->list, &dump_list);
		err = 0;
	}
	spin_unlock_irqrestore(&dump_list_lock, flags);

	return err;
}
EXPORT_SYMBOL_GPL(kmsg_dump_register);

/**
 * kmsg_dump_unregister - unregister a kmsg dumper.
 * @dumper: pointer to the kmsg_dumper structure
 *
 * Removes a dump device from the system. Returns zero on success and
 * %-EINVAL otherwise.
 */
int kmsg_dump_unregister(struct kmsg_dumper *dumper)
{
	unsigned long flags;
	int err = -EINVAL;

	spin_lock_irqsave(&dump_list_lock, flags);
	if (dumper->registered) {
		dumper->registered = 0;
		list_del_rcu(&dumper->list);
		err = 0;
	}
	spin_unlock_irqrestore(&dump_list_lock, flags);
	synchronize_rcu();

	return err;
}
EXPORT_SYMBOL_GPL(kmsg_dump_unregister);

static bool always_kmsg_dump;
module_param_named(always_kmsg_dump, always_kmsg_dump, bool, S_IRUGO | S_IWUSR);

/**
 * kmsg_dump - dump kernel log to kernel message dumpers.
 * @reason: the reason (oops, panic etc) for dumping
 *
 * Call each of the registered dumper's dump() callback, which can
 * retrieve the kmsg records with kmsg_dump_get_line() or
 * kmsg_dump_get_buffer().
 */
void kmsg_dump(enum kmsg_dump_reason reason)
{
	struct kmsg_dumper *dumper;
	unsigned long flags;

	if ((reason > KMSG_DUMP_OOPS) && !always_kmsg_dump)
		return;

	rcu_read_lock();
	list_for_each_entry_rcu(dumper, &dump_list, list) {
		if (dumper->max_reason && reason > dumper->max_reason)
			continue;

		/* initialize iterator with data about the stored records */
		dumper->active = true;

		raw_spin_lock_irqsave(&logbuf_lock, flags);
		dumper->cur_seq = clear_seq;
		dumper->cur_idx = clear_idx;
		dumper->next_seq = log_next_seq;
		dumper->next_idx = log_next_idx;
		raw_spin_unlock_irqrestore(&logbuf_lock, flags);

		/* invoke dumper which will iterate over records */
		dumper->dump(dumper, reason);

		/* reset iterator */
		dumper->active = false;
	}
	rcu_read_unlock();
}

/**
 * kmsg_dump_get_line_nolock - retrieve one kmsg log line (unlocked version)
 * @dumper: registered kmsg dumper
 * @syslog: include the "<4>" prefixes
 * @line: buffer to copy the line to
 * @size: maximum size of the buffer
 * @len: length of line placed into buffer
 *
 * Start at the beginning of the kmsg buffer, with the oldest kmsg
 * record, and copy one record into the provided buffer.
 *
 * Consecutive calls will return the next available record moving
 * towards the end of the buffer with the youngest messages.
 *
 * A return value of FALSE indicates that there are no more records to
 * read.
 *
 * The function is similar to kmsg_dump_get_line(), but grabs no locks.
 */
bool kmsg_dump_get_line_nolock(struct kmsg_dumper *dumper, bool syslog,
			       char *line, size_t size, size_t *len)
{
	struct printk_log *msg;
	size_t l = 0;
	bool ret = false;

	if (!dumper->active)
		goto out;

	if (dumper->cur_seq < log_first_seq) {
		/* messages are gone, move to first available one */
		dumper->cur_seq = log_first_seq;
		dumper->cur_idx = log_first_idx;
	}

	/* last entry */
	if (dumper->cur_seq >= log_next_seq)
		goto out;

	msg = log_from_idx(dumper->cur_idx);
	l = msg_print_text(msg, 0, syslog, line, size);

	dumper->cur_idx = log_next(dumper->cur_idx);
	dumper->cur_seq++;
	ret = true;
out:
	if (len)
		*len = l;
	return ret;
}

/**
 * kmsg_dump_get_line - retrieve one kmsg log line
 * @dumper: registered kmsg dumper
 * @syslog: include the "<4>" prefixes
 * @line: buffer to copy the line to
 * @size: maximum size of the buffer
 * @len: length of line placed into buffer
 *
 * Start at the beginning of the kmsg buffer, with the oldest kmsg
 * record, and copy one record into the provided buffer.
 *
 * Consecutive calls will return the next available record moving
 * towards the end of the buffer with the youngest messages.
 *
 * A return value of FALSE indicates that there are no more records to
 * read.
 */
bool kmsg_dump_get_line(struct kmsg_dumper *dumper, bool syslog,
			char *line, size_t size, size_t *len)
{
	unsigned long flags;
	bool ret;

	raw_spin_lock_irqsave(&logbuf_lock, flags);
	ret = kmsg_dump_get_line_nolock(dumper, syslog, line, size, len);
	raw_spin_unlock_irqrestore(&logbuf_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(kmsg_dump_get_line);

/**
 * kmsg_dump_get_buffer - copy kmsg log lines
 * @dumper: registered kmsg dumper
 * @syslog: include the "<4>" prefixes
 * @buf: buffer to copy the line to
 * @size: maximum size of the buffer
 * @len: length of line placed into buffer
 *
 * Start at the end of the kmsg buffer and fill the provided buffer
 * with as many of the the *youngest* kmsg records that fit into it.
 * If the buffer is large enough, all available kmsg records will be
 * copied with a single call.
 *
 * Consecutive calls will fill the buffer with the next block of
 * available older records, not including the earlier retrieved ones.
 *
 * A return value of FALSE indicates that there are no more records to
 * read.
 */
bool kmsg_dump_get_buffer(struct kmsg_dumper *dumper, bool syslog,
			  char *buf, size_t size, size_t *len)
{
	unsigned long flags;
	u64 seq;
	u32 idx;
	u64 next_seq;
	u32 next_idx;
	enum log_flags prev;
	size_t l = 0;
	bool ret = false;

	if (!dumper->active)
		goto out;

	raw_spin_lock_irqsave(&logbuf_lock, flags);
	if (dumper->cur_seq < log_first_seq) {
		/* messages are gone, move to first available one */
		dumper->cur_seq = log_first_seq;
		dumper->cur_idx = log_first_idx;
	}

	/* last entry */
	if (dumper->cur_seq >= dumper->next_seq) {
		raw_spin_unlock_irqrestore(&logbuf_lock, flags);
		goto out;
	}

	/* calculate length of entire buffer */
	seq = dumper->cur_seq;
	idx = dumper->cur_idx;
	prev = 0;
	while (seq < dumper->next_seq) {
		struct printk_log *msg = log_from_idx(idx);

		l += msg_print_text(msg, prev, true, NULL, 0);
		idx = log_next(idx);
		seq++;
		prev = msg->flags;
	}

	/* move first record forward until length fits into the buffer */
	seq = dumper->cur_seq;
	idx = dumper->cur_idx;
	prev = 0;
	while (l > size && seq < dumper->next_seq) {
		struct printk_log *msg = log_from_idx(idx);

		l -= msg_print_text(msg, prev, true, NULL, 0);
		idx = log_next(idx);
		seq++;
		prev = msg->flags;
	}

	/* last message in next interation */
	next_seq = seq;
	next_idx = idx;

	l = 0;
	while (seq < dumper->next_seq) {
		struct printk_log *msg = log_from_idx(idx);

		l += msg_print_text(msg, prev, syslog, buf + l, size - l);
		idx = log_next(idx);
		seq++;
		prev = msg->flags;
	}

	dumper->next_seq = next_seq;
	dumper->next_idx = next_idx;
	ret = true;
	raw_spin_unlock_irqrestore(&logbuf_lock, flags);
out:
	if (len)
		*len = l;
	return ret;
}
EXPORT_SYMBOL_GPL(kmsg_dump_get_buffer);

/**
 * kmsg_dump_rewind_nolock - reset the interator (unlocked version)
 * @dumper: registered kmsg dumper
 *
 * Reset the dumper's iterator so that kmsg_dump_get_line() and
 * kmsg_dump_get_buffer() can be called again and used multiple
 * times within the same dumper.dump() callback.
 *
 * The function is similar to kmsg_dump_rewind(), but grabs no locks.
 */
void kmsg_dump_rewind_nolock(struct kmsg_dumper *dumper)
{
	dumper->cur_seq = clear_seq;
	dumper->cur_idx = clear_idx;
	dumper->next_seq = log_next_seq;
	dumper->next_idx = log_next_idx;
}

/**
 * kmsg_dump_rewind - reset the interator
 * @dumper: registered kmsg dumper
 *
 * Reset the dumper's iterator so that kmsg_dump_get_line() and
 * kmsg_dump_get_buffer() can be called again and used multiple
 * times within the same dumper.dump() callback.
 */
void kmsg_dump_rewind(struct kmsg_dumper *dumper)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&logbuf_lock, flags);
	kmsg_dump_rewind_nolock(dumper);
	raw_spin_unlock_irqrestore(&logbuf_lock, flags);
}
EXPORT_SYMBOL_GPL(kmsg_dump_rewind);
