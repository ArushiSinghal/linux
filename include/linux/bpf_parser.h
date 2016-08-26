/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#ifndef _LINUX_BPF_PARSER_H
#define _LINUX_BPF_PARSER_H 1

#include <linux/bpf.h> /* for enum bpf_reg_type */
#include <linux/filter.h> /* for MAX_BPF_STACK */

struct reg_state {
	enum bpf_reg_type type;
	union {
		/* valid when type == CONST_IMM | PTR_TO_STACK | UNKNOWN_VALUE */
		s64 imm;

		/* valid when type == PTR_TO_PACKET* */
		struct {
			u32 id;
			u16 off;
			u16 range;
		};

		/* valid when type == CONST_PTR_TO_MAP | PTR_TO_MAP_VALUE |
		 *   PTR_TO_MAP_VALUE_OR_NULL
		 */
		struct bpf_map *map_ptr;
	};
};

enum bpf_stack_slot_type {
	STACK_INVALID,    /* nothing was stored in this stack slot */
	STACK_SPILL,      /* register spilled into stack */
	STACK_MISC	  /* BPF program wrote some data into this slot */
};

#define BPF_REG_SIZE 8	/* size of eBPF register in bytes */

/* state of the program:
 * type of all registers and stack info
 */
struct verifier_state {
	struct reg_state regs[MAX_BPF_REG];
	u8 stack_slot_type[MAX_BPF_STACK];
	struct reg_state spilled_regs[MAX_BPF_STACK / BPF_REG_SIZE];
};

/* linked list of verifier states used to prune search */
struct verifier_state_list {
	struct verifier_state state;
	struct verifier_state_list *next;
};

#define MAX_USED_MAPS 64 /* max number of maps accessed by one eBPF program */

struct verifier_env;
struct bpf_ext_parser_ops {
	int (*insn_hook)(struct verifier_env *env,
			 int insn_idx, int prev_insn_idx);
};

/* single container for all structs
 * one verifier_env per bpf_check() call
 */
struct verifier_env {
	struct bpf_prog *prog;		/* eBPF program being verified */
	struct verifier_stack_elem *head; /* stack of verifier states to be processed */
	int stack_size;			/* number of states to be processed */
	struct verifier_state cur_state; /* current verifier state */
	struct verifier_state_list **explored_states; /* search pruning optimization */
	const struct bpf_ext_parser_ops *pops; /* external parser ops */
	void *ppriv; /* pointer to external parser's private data */
	struct bpf_map *used_maps[MAX_USED_MAPS]; /* array of map's used by eBPF program */
	u32 used_map_cnt;		/* number of used maps */
	u32 id_gen;			/* used to generate unique reg IDs */
	bool allow_ptr_leaks;
};

int bpf_parse(struct bpf_prog *prog, const struct bpf_ext_parser_ops *pops,
	      void *ppriv);

#endif /* _LINUX_BPF_PARSER_H */
