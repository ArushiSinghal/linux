#ifndef _LINUX_STAT_H
#define _LINUX_STAT_H


#include <asm/stat.h>
#include <uapi/linux/stat.h>

#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

#define UTIME_NOW	((1l << 30) - 1l)
#define UTIME_OMIT	((1l << 30) - 2l)

#include <linux/types.h>
#include <linux/time.h>
#include <linux/uidgid.h>

struct kstat {
	u32		query_flags;		/* Operational flags */
#define KSTAT_QUERY_FLAGS (AT_FORCE_ATTR_SYNC | AT_NO_ATTR_SYNC)
	u32		request_mask;		/* What fields the user asked for */
	u32		result_mask;		/* What fields the user got */
	u32		information;
	u64		ioc_flags;		/* Inode flags (FS_IOC_GETFLAGS) */
	u64		ino;
	dev_t		dev;
	umode_t		mode;
	unsigned int	nlink;
	kuid_t		uid;
	kgid_t		gid;
	dev_t		rdev;
	loff_t		size;
	struct timespec	atime;
	struct timespec	mtime;
	struct timespec	ctime;
	struct timespec	btime;			/* File creation time */
	uint32_t	blksize;		/* Preferred I/O size */
	u64		blocks;
	u64		version;		/* Data version */
};

#endif
