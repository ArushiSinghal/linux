/*
 * Simple kernel driver to link kernel Ftrace and an STM device
 * Copyright (c) 2016, Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/stm.h>
#include <linux/trace_output_stm.h>

#define STM_FTRACE_NR_CHANNELS 1

static int stm_ftrace_link(struct stm_source_data *data);
static void stm_ftrace_unlink(struct stm_source_data *data);
static void stm_ftrace_write(struct stm_source_data *data, const char *buf,
			     unsigned int len, unsigned int chan);

static struct stm_ftrace ftrace = {
	.data	= {
		.name		= "ftrace",
		.nr_chans	= STM_FTRACE_NR_CHANNELS,
		.link		= stm_ftrace_link,
		.unlink		= stm_ftrace_unlink,
	},
	.write			= stm_ftrace_write,
};

/**
 * stm_ftrace_write() - write data to STM via 'stm_ftrace' source
 * @buf:	buffer containing the data packet
 * @len:	length of the data packet
 * @chan:	offset above the start channel number allocated to 'stm_ftrace'
 */
static void notrace stm_ftrace_write(struct stm_source_data *data,
				     const char *buf, unsigned int len,
				     unsigned int chan)
{
	stm_source_write(data, chan, buf, len);
}

static int stm_ftrace_link(struct stm_source_data *data)
{
	struct stm_ftrace *sf = container_of(data, struct stm_ftrace, data);

	trace_add_output(sf);

	return 0;
}

static void stm_ftrace_unlink(struct stm_source_data *data)
{
	trace_rm_output();
}

static int __init stm_ftrace_init(void)
{
	return stm_source_register_device(NULL, &ftrace.data);
}

static void __exit stm_ftrace_exit(void)
{
	stm_source_unregister_device(&ftrace.data);
}

module_init(stm_ftrace_init);
module_exit(stm_ftrace_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("stm_ftrace driver");
MODULE_AUTHOR("Chunyan Zhang <zhang.chunyan@linaro.org>");
