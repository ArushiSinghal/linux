/*
 * Ptrace test for VMX/VSX registers in the TM context
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "ptrace.h"
#include "ptrace-vsx.h"

int shm_id;
volatile int *cptr, *pptr;

extern void loadvsx(void *p, int tmp);
extern void storevsx(void *p, int tmp);

unsigned long fp_load[VEC_MAX];
unsigned long fp_store[VEC_MAX];
unsigned long fp_load_ckpt[VEC_MAX];
unsigned long fp_load_ckpt_new[VEC_MAX];

__attribute__((used)) void load_vsx(void)
{
	loadvsx(fp_load, 0);
}

__attribute__((used)) void load_vsx_ckpt(void)
{
	loadvsx(fp_load_ckpt, 0);
}

void tm_vsx(void)
{
	unsigned long result, texasr;
	int ret;

	cptr = (int *)shmat(shm_id, NULL, 0);
trans:
	asm __volatile__(
		"bl load_vsx_ckpt;"

		"1: ;"
		TBEGIN
		"beq 2f;"

		"bl load_vsx;"
		"b .;"

		TEND
		"li 0, 0;"
		"ori %[res], 0, 0;"
		"b 3f;"

		"2: ;"
		"li 0, 1;"
		"ori %[res], 0, 0;"
		"mfspr %[texasr], %[sprn_texasr];"

		"3: ;"
		: [res] "=r" (result), [texasr] "=r" (texasr)
		: [fp_load] "r" (fp_load), [fp_load_ckpt] "r" (fp_load_ckpt), [sprn_texasr] "i"  (SPRN_TEXASR)
		: "memory", "r0", "r1", "r2", "r3", "r4", "r8", "r9", "r10", "r11"
		);

	if (result) {
		if (!cptr[0])
			goto trans;
		shmdt((void *)cptr);

		storevsx(fp_store, 0);
		ret = compare_vsx_vmx(fp_store, fp_load_ckpt_new);
		if (ret)
			exit(1);
		exit(0);
	}
	exit(1);
}

int trace_tm_vsx(pid_t child)
{
	unsigned long vsx[VSX_MAX];
	unsigned long vmx[VMX_MAX + 2][2];
	int ret;

	sleep(1);
	ret = start_trace(child);
	if (ret)
		return TEST_FAIL;

	ret = show_vsx(child, vsx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vsx(vsx, fp_load);
	if (ret)
		return TEST_FAIL;

	ret = show_vmx(child, vmx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vmx(vmx, fp_load);
	if (ret)
		return TEST_FAIL;

	ret = show_vsx_ckpt(child, vsx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vsx(vsx, fp_load_ckpt);
	if (ret)
		return TEST_FAIL;

	ret = show_vmx_ckpt(child, vmx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vmx(vmx, fp_load_ckpt);
	if (ret)
		return TEST_FAIL;

	memset(vsx, 0, sizeof(vsx));
	memset(vmx, 0, sizeof(vmx));

	load_vsx_vmx(fp_load_ckpt_new, vsx, vmx);

	ret = write_vsx_ckpt(child, vsx);
	if (ret)
		return TEST_FAIL;

	ret = write_vmx_ckpt(child, vmx);
	if (ret)
		return TEST_FAIL;

	ret = stop_trace(child);
	if (ret)
		return TEST_FAIL;

	return TEST_PASS;
}

int ptrace_tm_vsx(void)
{
	pid_t pid;
	int ret, status, i;

	SKIP_IF(!((long)get_auxv_entry(AT_HWCAP2) & PPC_FEATURE2_HTM));
	shm_id = shmget(IPC_PRIVATE, sizeof(int) * 1, 0777|IPC_CREAT);

	for(i = 0; i < 128; i++) {
		fp_load[i] = 1 + rand();
		fp_load_ckpt[i] = 1 + 2 * rand();
		fp_load_ckpt_new[i] = 1 + 3 * rand();
	}

	pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		return TEST_FAIL;
	}

	if (pid == 0)
		tm_vsx();

	if (pid) {
		pptr = (int *)shmat(shm_id, NULL, 0);
		ret = trace_tm_vsx(pid);
		if (ret) {
			kill(pid, SIGKILL);
			return TEST_FAIL;
		}

		pptr[0] = 1;
		shmdt((void *)pptr);
		ret = wait(&status);
		if (ret != pid) {
			printf("Child's exit status not captured\n");
			return TEST_FAIL;
		}

		if (WIFEXITED(status)) {
			if(WEXITSTATUS(status))
				return TEST_FAIL;
		}
		return TEST_PASS;
	}
	return TEST_PASS;
}

int main(int argc, char *argv[])
{
	return test_harness(ptrace_tm_vsx, "ptrace_tm_vsx");
}
