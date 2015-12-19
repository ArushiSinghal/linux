#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/statfs.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <fcntl.h>

#include <linux/magic.h>
#include <linux/sched.h>

#include "../kselftest.h"

#define STACK_SIZE 65536

static char root_cgroup[4096];

#define CHILDREN_COUNT 2
typedef struct {
	int pid;
	uint8_t *stack;
	int start_semfd;
	int end_semfd;
} cgroupns_child_t;
cgroupns_child_t children[CHILDREN_COUNT];

typedef enum {
	UNSHARE_CGROUPNS,
	JOIN_CGROUPNS,
	CHECK_CGROUP,
	CHECK_CGROUP_WITH_ROOT_PREFIX,
	MOVE_CGROUP,
	MOVE_CGROUP_WITH_ROOT_PREFIX,
} cgroupns_action_t;

static const struct {
	int actor_pid;
	cgroupns_action_t action;
	int target_pid;
	char *path;
} cgroupns_tests[] = {
	{ 0, CHECK_CGROUP_WITH_ROOT_PREFIX, -1, ""},
	{ 0, CHECK_CGROUP_WITH_ROOT_PREFIX, 0, ""},
	{ 0, CHECK_CGROUP_WITH_ROOT_PREFIX, 1, ""},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, -1, ""},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 0, ""},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 1, ""},

	{ 0, UNSHARE_CGROUPNS, -1, NULL},

	{ 0, CHECK_CGROUP, -1, "/"},
	{ 0, CHECK_CGROUP, 0, "/"},
	{ 0, CHECK_CGROUP, 1, "/"},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, -1, ""},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 0, ""},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 1, ""},

	{ 1, UNSHARE_CGROUPNS, -1, NULL},

	{ 0, CHECK_CGROUP, -1, "/"},
	{ 0, CHECK_CGROUP, 0, "/"},
	{ 0, CHECK_CGROUP, 1, "/"},
	{ 1, CHECK_CGROUP, -1, "/"},
	{ 1, CHECK_CGROUP, 0, "/"},
	{ 1, CHECK_CGROUP, 1, "/"},

	{ 0, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-a"},
	{ 1, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-b"},

	{ 0, CHECK_CGROUP, -1, "/cgroup-a"},
	{ 0, CHECK_CGROUP, 0, "/cgroup-a"},
	{ 0, CHECK_CGROUP, 1, "/cgroup-b"},
	{ 1, CHECK_CGROUP, -1, "/cgroup-b"},
	{ 1, CHECK_CGROUP, 0, "/cgroup-a"},
	{ 1, CHECK_CGROUP, 1, "/cgroup-b"},

	{ 0, UNSHARE_CGROUPNS, -1, NULL},
	{ 1, UNSHARE_CGROUPNS, -1, NULL},

	{ 0, CHECK_CGROUP, -1, "/"},
	{ 0, CHECK_CGROUP, 0, "/"},
	{ 0, CHECK_CGROUP, 1, "/../cgroup-b"},
	{ 1, CHECK_CGROUP, -1, "/"},
	{ 1, CHECK_CGROUP, 0, "/../cgroup-a"},
	{ 1, CHECK_CGROUP, 1, "/"},

	{ 0, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-a/sub1-a"},
	{ 1, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-b/sub1-b"},

	{ 0, CHECK_CGROUP, 0, "/sub1-a"},
	{ 0, CHECK_CGROUP, 1, "/../cgroup-b/sub1-b"},
	{ 1, CHECK_CGROUP, 0, "/../cgroup-a/sub1-a"},
	{ 1, CHECK_CGROUP, 1, "/sub1-b"},

	{ 0, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-a/sub1-a/sub2-a"},
	{ 1, CHECK_CGROUP, 0, "/../cgroup-a/sub1-a/sub2-a"},
	{ 0, CHECK_CGROUP, 1, "/../cgroup-b/sub1-b"},
	{ 0, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-a/sub1-a/sub2-a/sub3-a"},
	{ 1, CHECK_CGROUP, 0, "/../cgroup-a/sub1-a/sub2-a/sub3-a"},
	{ 0, CHECK_CGROUP, 1, "/../cgroup-b/sub1-b"},
	{ 0, MOVE_CGROUP_WITH_ROOT_PREFIX, -1, "cgroup-a/sub1-a/sub2-a/sub3-a/sub4-a"},
	{ 1, CHECK_CGROUP, 0, "/../cgroup-a/sub1-a/sub2-a/sub3-a/sub4-a"},
	{ 0, CHECK_CGROUP, 1, "/../cgroup-b/sub1-b"},

	{ 1, UNSHARE_CGROUPNS, -1, NULL},
	{ 1, CHECK_CGROUP, 0, "/../../cgroup-a/sub1-a/sub2-a/sub3-a/sub4-a"},
	{ 0, UNSHARE_CGROUPNS, -1, NULL},
	{ 0, CHECK_CGROUP, 1, "/../../../../../cgroup-b/sub1-b"},

	{ 0, JOIN_CGROUPNS, -1, NULL},
	{ 1, JOIN_CGROUPNS, -1, NULL},

	{ 0, CHECK_CGROUP_WITH_ROOT_PREFIX, 0, "/cgroup-a/sub1-a/sub2-a/sub3-a/sub4-a"},
	{ 0, CHECK_CGROUP_WITH_ROOT_PREFIX, 1, "/cgroup-b/sub1-b"},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 0, "/cgroup-a/sub1-a/sub2-a/sub3-a/sub4-a"},
	{ 1, CHECK_CGROUP_WITH_ROOT_PREFIX, 1, "/cgroup-b/sub1-b"},
};
#define cgroupns_tests_len (sizeof(cgroupns_tests) / sizeof(cgroupns_tests[0]))

static void
get_cgroup(pid_t pid, char *path, size_t len)
{
	char proc_path[4096];
	char *line;
	FILE *f;

	if (pid > 0) {
		sprintf(proc_path, "/proc/%d/cgroup", pid);
	} else {
		sprintf(proc_path, "/proc/self/cgroup");
	}

	f = fopen(proc_path, "r");
	if (!f) {
		printf("FAIL: cannot open %s\n", proc_path);
		ksft_exit_fail();
	}
	line = malloc(4096);
	line = fgets(line, 4096, f);
	if (!line) {
		printf("FAIL: %s empty\n", proc_path);
		ksft_exit_fail();
	}
	line[strcspn(line, "\n")] = 0;
	if (strncmp(line, "0::", 3) != 0) {
		printf("FAIL: cannot parse %s\n", proc_path);
		ksft_exit_fail();
	}
	strncpy(path, line+3, len);

	free(line);
	fclose(f);
}

static void
move_cgroup(pid_t target_pid, int prefix, char *cgroup)
{
	char knob_dir[4096];
	char knob_path[4096];
	char buf[128];
	FILE *f;
	int ret;

	if (prefix) {
		sprintf(knob_dir, "/sys/fs/cgroup/%s/%s", root_cgroup, cgroup);
		sprintf(knob_path, "/sys/fs/cgroup/%s/%s/cgroup.procs", root_cgroup, cgroup);
	} else {
		sprintf(knob_dir, "/sys/fs/cgroup/%s", cgroup);
		sprintf(knob_path, "/sys/fs/cgroup/%s/cgroup.procs", cgroup);
	}

	mkdir(knob_dir, 0755);

	sprintf(buf, "%d\n", target_pid);

	f = fopen(knob_path, "w");
	ret = fwrite(buf, strlen(buf), 1, f);
	if (ret != 1) {
		printf("FAIL: cannot write to %s (ret=%d)\n", knob_path, ret);
		ksft_exit_fail();
	}
	fclose(f);
}

static int
child_func(void *arg)
{
	uintptr_t id = (uintptr_t) arg;
	char child_cgroup[4096];
	char expected_cgroup[4096];
	char process_name[128];
	char proc_path[128];
	int step;
	int ret;
	int nsfd;

	for (step = 0; step < cgroupns_tests_len; step++) {
		uint64_t counter = 0;
		int target_pid;

		/* wait a signal from the parent process before starting this step */
		ret = read(children[id].start_semfd, &counter, sizeof(counter));
		if (ret != sizeof(counter)) {
			printf("FAIL: cannot read semaphore\n");
			ksft_exit_fail();
		}

		/* only one process will do this step */
		if (cgroupns_tests[step].actor_pid == id)
		switch (cgroupns_tests[step].action) {
			case UNSHARE_CGROUPNS:
				printf("child process #%d: unshare cgroupns\n", id);
				ret = unshare(CLONE_NEWCGROUP);
				if (ret != 0) {
					printf("FAIL: cannot unshare cgroupns\n");
					ksft_exit_fail();
				}
				break;

			case JOIN_CGROUPNS:
				printf("child process #%d: join parent cgroupns\n", id);

				sprintf(proc_path, "/proc/%d/ns/cgroup", getppid());
				nsfd = open(proc_path, 0);
				ret = setns(nsfd, CLONE_NEWCGROUP);
				if (ret != 0) {
					printf("FAIL: cannot join cgroupns\n");
					ksft_exit_fail();
				}
				close(nsfd);
				break;

			case CHECK_CGROUP:
			case CHECK_CGROUP_WITH_ROOT_PREFIX:
				if (cgroupns_tests[step].action == CHECK_CGROUP)
					sprintf(expected_cgroup, "%s", cgroupns_tests[step].path);
				else
					sprintf(expected_cgroup, "%s%s", root_cgroup, cgroupns_tests[step].path);

				if (cgroupns_tests[step].target_pid >= 0) {
					target_pid = children[cgroupns_tests[step].target_pid].pid;
					sprintf(process_name, "#%d (pid=%d)",
					        cgroupns_tests[step].target_pid, target_pid);
				} else {
					target_pid = 0;
					sprintf(process_name, "#self (pid=%d)", getpid());
				}

				printf("child process #%d: check that process %s has cgroup %s\n",
				       id, process_name, expected_cgroup);

				get_cgroup(target_pid, child_cgroup, sizeof(child_cgroup));

				if (strcmp(child_cgroup, expected_cgroup) != 0) {
					printf("FAIL: child has cgroup %s\n", child_cgroup);
					ksft_exit_fail();
				}

				break;

			case MOVE_CGROUP:
			case MOVE_CGROUP_WITH_ROOT_PREFIX:
				if (cgroupns_tests[step].target_pid >= 0) {
					target_pid = children[cgroupns_tests[step].target_pid].pid;
					sprintf(process_name, "#%d (pid=%d)",
					        cgroupns_tests[step].target_pid, target_pid);
				} else {
					target_pid = getpid();
					sprintf(process_name, "#self (pid=%d)", getpid());
				}

				printf("child process #%d: move process %s to cgroup %s\n",
				       id, process_name, cgroupns_tests[step].path);

				move_cgroup(target_pid,
				            cgroupns_tests[step].action == MOVE_CGROUP_WITH_ROOT_PREFIX,
				            cgroupns_tests[step].path);
				break;

			default:
				printf("FAIL: invalid action\n");
				ksft_exit_fail();
		}


		/* signal the parent process we've finished this step */
		counter = 1;
		ret = write(children[id].end_semfd, &counter, sizeof(counter));
		if (ret != sizeof(counter)) {
			printf("FAIL: cannot write semaphore\n");
			ksft_exit_fail();
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct statfs fs;
	char child_cgroup[4096];
	int ret;
	int status;
	uintptr_t i;
	int step;

	if (statfs("/sys/fs/cgroup/", &fs) < 0) {
		printf("FAIL: statfs\n");
		ksft_exit_fail();
	}

	if (fs.f_type != (typeof(fs.f_type)) CGROUP2_SUPER_MAGIC) {
		printf("FAIL: this test is for Linux >= 4.4 with cgroup2 mounted\n");
		ksft_exit_fail();
	}

	get_cgroup(0, root_cgroup, sizeof(root_cgroup));
	printf("current cgroup: %s\n", root_cgroup);

	for (i = 0; i < CHILDREN_COUNT; i++) {
		children[i].start_semfd = eventfd(0, EFD_SEMAPHORE);
		children[i].end_semfd = eventfd(0, EFD_SEMAPHORE);

		children[i].stack = malloc(STACK_SIZE);
		if (!children[i].stack) {
			printf("FAIL: cannot allocate stack\n");
			ksft_exit_fail();
		}
	}

	for (i = 0; i < CHILDREN_COUNT; i++) {
		children[i].pid = clone(child_func, children[i].stack + STACK_SIZE,
		                        SIGCHLD|CLONE_VM|CLONE_FILES, (void *)i);
	}

	for (step = 0; step < cgroupns_tests_len; step++) {
		uint64_t counter = 1;

		/* signal the child processes they can start the current step */
		for (i = 0; i < CHILDREN_COUNT; i++) {
			ret = write(children[i].start_semfd, &counter, sizeof(counter));
			if (ret != sizeof(counter)) {
				printf("FAIL: cannot write semaphore\n");
				ksft_exit_fail();
			}
		}

		/* wait until all child processes finished the current step */
		for (i = 0; i < CHILDREN_COUNT; i++) {
			ret = read(children[i].end_semfd, &counter, sizeof(counter));
			if (ret != sizeof(counter)) {
				printf("FAIL: cannot read semaphore\n");
				ksft_exit_fail();
			}
		}
	}

	for (i = 0; i < CHILDREN_COUNT; i++) {
		ret = wait(&status);
		if (ret == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("FAIL: cannot wait child\n");
			ksft_exit_fail();
		}
	}

	printf("SUCCESS\n");
	return ksft_exit_pass();
}
