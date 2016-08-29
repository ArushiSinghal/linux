#ifndef _LINUX_TRACE_H
#define _LINUX_TRACE_H

#include <linux/ring_buffer.h>
struct trace_array;

#ifdef CONFIG_TRACING
/*
 * The trace export - an export of function traces.  Every ftrace_ops
 * has at least one export which would output function traces to ring
 * buffer.
 *
 * next		- pointer to the next trace_export
 * tr		- the trace_array this export belongs to
 * commit	- commit the traces to ring buffer and/or some other places
 * write	- copy traces which have been delt with ->commit() to
 *		  the destination
 */
struct trace_export {
	struct trace_export __rcu	*next;
	void (*commit)(struct trace_array *, struct ring_buffer_event *);
	void (*write)(const char *, unsigned int);
};

int register_ftrace_export(struct trace_export *export);
int unregister_ftrace_export(struct trace_export *export);

#endif	/* CONFIG_TRACING */

#endif	/* _LINUX_TRACE_H */
