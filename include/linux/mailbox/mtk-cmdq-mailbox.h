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

#ifndef __MTK_CMDQ_MAILBOX_H__
#define __MTK_CMDQ_MAILBOX_H__

#include <linux/slab.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#define CMDQ_INST_SIZE			8 /* instruction is 64-bit */
#define CMDQ_OP_CODE_SHIFT		24
#define CMDQ_JUMP_PASS			CMDQ_INST_SIZE

#define CMDQ_WFE_UPDATE			BIT(31)
#define CMDQ_WFE_WAIT			BIT(15)
#define CMDQ_WFE_WAIT_VALUE		0x1

/*
 * CMDQ_CODE_MASK:
 *   set write mask
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
 */
enum cmdq_code {
	CMDQ_CODE_MASK = 0x02,
	CMDQ_CODE_WRITE = 0x04,
	CMDQ_CODE_JUMP = 0x10,
	CMDQ_CODE_WFE = 0x20,
	CMDQ_CODE_EOC = 0x40,
};

struct cmdq_task_cb {
	cmdq_async_flush_cb	cb;
	void			*data;
};

struct cmdq_pkt {
	void			*va_base;
	size_t			cmd_buf_size; /* command occupied size */
	size_t			buf_size; /* real buffer size */
	struct cmdq_task_cb	cb;
};

#endif /* __MTK_CMDQ_MAILBOX_H__ */
