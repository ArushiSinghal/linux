/*
 * Copyright(c) 2015 EZchip Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#ifndef SOC_NPS_COMMON_H
#define SOC_NPS_COMMON_H

#define NPS_HOST_REG_BASE			0xF6000000
#define CTOP_INST_MOV2B_FLIP_R3_B1_B2_INST	0x5B60
#define CTOP_INST_MOV2B_FLIP_R3_B1_B2_LIMM	0x00010422

#ifndef __ASSEMBLY__
/* CPU global ID */
struct global_id {
	union {
		struct {
#ifdef CONFIG_EZNPS_MTM_EXT
			u32 __reserved:20, cluster:4, core:4, thread:4;
#else
			u32 __reserved:24, cluster:4, core:4;
#endif
		};
		u32 value;
	};
};

/*
 * Convert logical to physical CPU IDs
 *
 * The conversion swap bits 1 and 2 of cluster id (out of 4 bits)
 * Now quad of logical clusters id's are adjacent physically,
 * and not like the id's physically came with each cluster.
 * Below table is 4x4 mesh of core clusters as it layout on chip.
 * Cluster ids are in format: logical (physical)
 *
 *    -----------------   ------------------
 * 3 |  5 (3)   7 (7)  | | 13 (11)   15 (15)|
 *
 * 2 |  4 (2)   6 (6)  | | 12 (10)   14 (14)|
 *    -----------------   ------------------
 * 1 |  1 (1)   3 (5)  | |  9  (9)   11 (13)|
 *
 * 0 |  0 (0)   2 (4)  | |  8  (8)   10 (12)|
 *    -----------------   ------------------
 *       0       1            2        3
 */
static inline int nps_cluster_logic_to_phys(int cluster)
{
	 __asm__ __volatile__(
	"       mov r3,%0\n"
	"       .short %1\n"
	"       .word %2\n"
	"       mov %0,r3\n"
	: "+r"(cluster)
	: "i"(CTOP_INST_MOV2B_FLIP_R3_B1_B2_INST),
	  "i"(CTOP_INST_MOV2B_FLIP_R3_B1_B2_LIMM)
	: "r3");

	return cluster;
}

#define NPS_CPU_TO_CLUSTER_NUM(cpu) \
	({ struct global_id gid; gid.value = cpu; \
		nps_cluster_logic_to_phys(gid.cluster); })

struct nps_host_reg_address {
	union {
		struct {
			u32 base:8, cl_x:4, cl_y:4,
			blkid:6, reg:8, __reserved:2;
		};
		u32 value;
	};
};

struct nps_host_reg_address_non_cl {
	union {
		struct {
			u32 base:7, blkid:11, reg:12, __reserved:2;
		};
		u32 value;
	};
};

static inline void *nps_host_reg_non_cl(u32 blkid, u32 reg)
{
	struct nps_host_reg_address_non_cl reg_address;

	reg_address.value = NPS_HOST_REG_BASE;
	reg_address.blkid = blkid;
	reg_address.reg = reg;

	return (void *)reg_address.value;
}

static inline void *nps_host_reg(u32 cpu, u32 blkid, u32 reg)
{
	struct nps_host_reg_address reg_address;
	u32 cl = NPS_CPU_TO_CLUSTER_NUM(cpu);

	reg_address.value = NPS_HOST_REG_BASE;
	reg_address.cl_x  = (cl >> 2) & 0x3;
	reg_address.cl_y  = cl & 0x3;
	reg_address.blkid = blkid;
	reg_address.reg   = reg;

	return (void *)reg_address.value;
}
#endif /* __ASSEMBLY__ */

#endif /* SOC_NPS_COMMON_H */
