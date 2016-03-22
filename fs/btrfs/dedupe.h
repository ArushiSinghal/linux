/*
 * Copyright (C) 2015 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_DEDUPE__
#define __BTRFS_DEDUPE__

#include <linux/btrfs.h>
#include <linux/wait.h>
#include <crypto/hash.h>

/*
 * Dedup storage backend
 * On disk is persist storage but overhead is large
 * In memory is fast but will lose all its hash on umount
 */
#define BTRFS_DEDUPE_BACKEND_INMEMORY		0
#define BTRFS_DEDUPE_BACKEND_ONDISK		1
#define BTRFS_DEDUPE_BACKEND_COUNT		2

/* Dedup block size limit and default value */
#define BTRFS_DEDUPE_BLOCKSIZE_MAX	(8 * 1024 * 1024)
#define BTRFS_DEDUPE_BLOCKSIZE_MIN	(16 * 1024)
#define BTRFS_DEDUPE_BLOCKSIZE_DEFAULT	(128 * 1024)

/* Hash algorithm, only support SHA256 yet */
#define BTRFS_DEDUPE_HASH_SHA256		0

static int btrfs_dedupe_sizes[] = { 32 };

/*
 * For caller outside of dedup.c
 *
 * Different dedupe backends should have their own hash structure
 */
struct btrfs_dedupe_hash {
	u64 bytenr;
	u32 num_bytes;

	/* last field is a variable length array of dedupe hash */
	u8 hash[];
};

struct btrfs_dedupe_info {
	/* dedupe blocksize */
	u64 blocksize;
	u16 backend;
	u16 hash_type;

	struct crypto_shash *dedupe_driver;
	struct mutex lock;

	/* following members are only used in in-memory dedupe mode */
	struct rb_root hash_root;
	struct rb_root bytenr_root;
	struct list_head lru_list;
	u64 limit_nr;
	u64 current_nr;
};

struct btrfs_trans_handle;

static inline int btrfs_dedupe_hash_hit(struct btrfs_dedupe_hash *hash)
{
	return (hash && hash->bytenr);
}

int btrfs_dedupe_hash_size(u16 type);
struct btrfs_dedupe_hash *btrfs_dedupe_alloc_hash(u16 type);

/*
 * Initial inband dedupe info
 * Called at dedupe enable time.
 */
int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info, u16 type, u16 backend,
			u64 blocksize, u64 limit_nr);

/*
 * Disable dedupe and invalidate all its dedupe data.
 * Called at dedupe disable time.
 */
int btrfs_dedupe_disable(struct btrfs_fs_info *fs_info);

/*
 * Calculate hash for dedup.
 * Caller must ensure [start, start + dedupe_bs) has valid data.
 */
int btrfs_dedupe_calc_hash(struct btrfs_fs_info *fs_info,
			   struct inode *inode, u64 start,
			   struct btrfs_dedupe_hash *hash);

/*
 * Search for duplicated extents by calculated hash
 * Caller must call btrfs_dedupe_calc_hash() first to get the hash.
 *
 * @inode: the inode for we are writing
 * @file_pos: offset inside the inode
 * As we will increase extent ref immediately after a hash match,
 * we need @file_pos and @inode in this case.
 *
 * Return > 0 for a hash match, and the extent ref will be
 * *INCREASED*, and hash->bytenr/num_bytes will record the existing
 * extent data.
 * Return 0 for a hash miss. Nothing is done
 */
int btrfs_dedupe_search(struct btrfs_fs_info *fs_info,
			struct inode *inode, u64 file_pos,
			struct btrfs_dedupe_hash *hash);

/* Add a dedupe hash into dedupe info */
int btrfs_dedupe_add(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_dedupe_hash *hash);

/* Remove a dedupe hash from dedupe info */
int btrfs_dedupe_del(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info, u64 bytenr);
#endif
