/*
 * Copyright 2007 Andi Kleen, SUSE Labs.
 * Subject to the GPL, v.2
 *
 * This contains most of the x86 vDSO kernel-side code.
 */
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/random.h>
#include <linux/elf.h>
#include <linux/cpu.h>
#include <linux/ptrace.h>
#include <linux/pagemap.h>
#include <linux/mount.h>
#include <linux/ramfs.h>
#include <linux/file.h>
#include <asm/pvclock.h>
#include <asm/vgtod.h>
#include <asm/proto.h>
#include <asm/vdso.h>
#include <asm/vvar.h>
#include <asm/page.h>
#include <asm/desc.h>
#include <asm/cpufeature.h>

#if defined(CONFIG_X86_64)
unsigned int __read_mostly vdso64_enabled = 1;
#endif

void __init init_vdso_image(const struct vdso_image *image)
{
	BUG_ON(image->size % PAGE_SIZE != 0);

	apply_alternatives((struct alt_instr *)(image->data + image->alt),
			   (struct alt_instr *)(image->data + image->alt +
						image->alt_len));
}

static struct vfsmount *vdso_mnt;
struct file *vdso_file_64;

struct linux_binprm;

/*
 * Put the vdso above the (randomized) stack with another randomized
 * offset.  This way there is no hole in the middle of address space.
 * To save memory make sure it is still in the same PTE as the stack
 * top.  This doesn't give that many random bits.
 *
 * Note that this algorithm is imperfect: the distribution of the vdso
 * start address within a PMD is biased toward the end.
 *
 * Only used for the 64-bit and x32 vdsos.
 */
static unsigned long vdso_addr(unsigned long start, unsigned len)
{
#ifdef CONFIG_X86_32
	return 0;
#else
	unsigned long addr, end;
	unsigned offset;

	/*
	 * Round up the start address.  It can start out unaligned as a result
	 * of stack start randomization.
	 */
	start = PAGE_ALIGN(start);

	/* Round the lowest possible end address up to a PMD boundary. */
	end = (start + len + PMD_SIZE - 1) & PMD_MASK;
	if (end >= TASK_SIZE_MAX)
		end = TASK_SIZE_MAX;
	end -= len;

	if (end > start) {
		offset = get_random_int() % (((end - start) >> PAGE_SHIFT) + 1);
		addr = start + (offset << PAGE_SHIFT);
	} else {
		addr = start;
	}

	/*
	 * Forcibly align the final address in case we have a hardware
	 * issue that requires alignment for performance reasons.
	 */
	addr = align_vdso_addr(addr);

	return addr;
#endif
}

static int vdso_fault(const struct vm_special_mapping *sm,
		      struct vm_area_struct *vma, struct vm_fault *vmf)
{
	const struct vdso_image *image = vma->vm_mm->context.vdso_image;

	if (!image || (vmf->pgoff << PAGE_SHIFT) >= image->size)
		return VM_FAULT_SIGBUS;

	vmf->page = virt_to_page(image->data + (vmf->pgoff << PAGE_SHIFT));
	get_page(vmf->page);
	return 0;
}

static void vdso_fix_landing(const struct vdso_image *image,
		struct vm_area_struct *new_vma)
{
#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
	if (in_ia32_syscall() && image == &vdso_image_32) {
		struct pt_regs *regs = current_pt_regs();
		unsigned long vdso_land = image->sym_int80_landing_pad;
		unsigned long old_land_addr = vdso_land +
			(unsigned long)current->mm->context.vdso;

		/* Fixing userspace landing - look at do_fast_syscall_32 */
		if (regs->ip == old_land_addr)
			regs->ip = new_vma->vm_start + vdso_land;
	}
#endif
}

static int vdso_mremap(const struct vm_special_mapping *sm,
		struct vm_area_struct *new_vma)
{
	unsigned long new_size = new_vma->vm_end - new_vma->vm_start;
	const struct vdso_image *image = current->mm->context.vdso_image;

	if (image->size != new_size)
		return -EINVAL;

	if (WARN_ON_ONCE(current->mm != new_vma->vm_mm))
		return -EFAULT;

	vdso_fix_landing(image, new_vma);
	current->mm->context.vdso = (void __user *)new_vma->vm_start;

	return 0;
}

static int vvar_fault(const struct vm_special_mapping *sm,
		      struct vm_area_struct *vma, struct vm_fault *vmf)
{
	const struct vdso_image *image = vma->vm_mm->context.vdso_image;
	long sym_offset;
	int ret = -EFAULT;

	if (!image)
		return VM_FAULT_SIGBUS;

	sym_offset = (long)(vmf->pgoff << PAGE_SHIFT) +
		image->sym_vvar_start;

	/*
	 * Sanity check: a symbol offset of zero means that the page
	 * does not exist for this vdso image, not that the page is at
	 * offset zero relative to the text mapping.  This should be
	 * impossible here, because sym_offset should only be zero for
	 * the page past the end of the vvar mapping.
	 */
	if (sym_offset == 0)
		return VM_FAULT_SIGBUS;

	if (sym_offset == image->sym_vvar_page) {
		ret = vm_insert_pfn(vma, (unsigned long)vmf->virtual_address,
				    __pa_symbol(&__vvar_page) >> PAGE_SHIFT);
	} else if (sym_offset == image->sym_pvclock_page) {
		struct pvclock_vsyscall_time_info *pvti =
			pvclock_pvti_cpu0_va();
		if (pvti && vclock_was_used(VCLOCK_PVCLOCK)) {
			ret = vm_insert_pfn(
				vma,
				(unsigned long)vmf->virtual_address,
				__pa(pvti) >> PAGE_SHIFT);
		}
	}

	if (ret == 0 || ret == -EBUSY)
		return VM_FAULT_NOPAGE;

	return VM_FAULT_SIGBUS;
}

static int map_vdso(const struct vdso_image *image, bool calculate_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long addr, text_start;
	int ret = 0;

	static const struct vm_special_mapping vdso_mapping = {
		.name = "[vdso]",
		.fault = vdso_fault,
		.mremap = vdso_mremap,
	};
	static const struct vm_special_mapping vvar_mapping = {
		.name = "[vvar]",
		.fault = vvar_fault,
	};

	if (calculate_addr) {
		addr = vdso_addr(current->mm->start_stack,
				 image->size - image->sym_vvar_start);
	} else {
		addr = 0;
	}

	if (down_write_killable(&mm->mmap_sem))
		return -EINTR;

	addr = get_unmapped_area(NULL, addr,
				 image->size - image->sym_vvar_start, 0, 0);
	if (IS_ERR_VALUE(addr)) {
		ret = addr;
		goto up_fail;
	}

	text_start = addr - image->sym_vvar_start;
	current->mm->context.vdso = (void __user *)text_start;
	current->mm->context.vdso_image = image;

	/*
	 * MAYWRITE to allow gdb to COW and set breakpoints
	 */
	if (__kuid_val(task_uid(current)) == 1001) {
		unsigned long n_addr = mmap_region(vdso_file_64, text_start,
				image->size, VM_READ|VM_EXEC|
				VM_DONTEXPAND|VM_SOFTDIRTY|
				VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC, 0);
		if (text_start != n_addr) {
			pr_err("Failed to mmap vdso file at %lx, mmap_region returned %lx\n",
					text_start, n_addr);
			goto old_way;
		}
		vma = find_vma(mm, text_start);
		if (IS_ERR(vma) || vma->vm_start != text_start) {
			pr_err("Failed to find vdso mapped vma at %lx\n",
					text_start);
			goto old_way;
		}
	} else {
old_way:
		vma = _install_special_mapping(mm,
				text_start,
				image->size,
				VM_READ|VM_EXEC|
				VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
				&vdso_mapping);
	}

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto up_fail;
	}

	vma = _install_special_mapping(mm,
				       addr,
				       -image->sym_vvar_start,
				       VM_READ|VM_MAYREAD|VM_IO|VM_DONTDUMP|
				       VM_PFNMAP,
				       &vvar_mapping);

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto up_fail;
	}

up_fail:
	if (ret)
		current->mm->context.vdso = NULL;

	up_write(&mm->mmap_sem);
	return ret;
}

#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
static int load_vdso32(void)
{
	if (vdso32_enabled != 1)  /* Other values all mean "disabled" */
		return 0;

	return map_vdso(&vdso_image_32, false);
}
#endif

#ifdef CONFIG_X86_64
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	if (!vdso64_enabled)
		return 0;

	return map_vdso(&vdso_image_64, true);
}

#ifdef CONFIG_COMPAT
int compat_arch_setup_additional_pages(struct linux_binprm *bprm,
				       int uses_interp)
{
#ifdef CONFIG_X86_X32_ABI
	if (test_thread_flag(TIF_X32)) {
		if (!vdso64_enabled)
			return 0;

		return map_vdso(&vdso_image_x32, true);
	}
#endif
#ifdef CONFIG_IA32_EMULATION
	return load_vdso32();
#else
	return 0;
#endif
}
#endif
#else
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	return load_vdso32();
}
#endif

#ifdef CONFIG_X86_64
static __init int vdso_setup(char *s)
{
	vdso64_enabled = simple_strtoul(s, NULL, 0);
	return 0;
}
__setup("vdso=", vdso_setup);
#endif

#ifdef CONFIG_X86_64
static void vgetcpu_cpu_init(void *arg)
{
	int cpu = smp_processor_id();
	struct desc_struct d = { };
	unsigned long node = 0;
#ifdef CONFIG_NUMA
	node = cpu_to_node(cpu);
#endif
	if (static_cpu_has(X86_FEATURE_RDTSCP))
		write_rdtscp_aux((node << 12) | cpu);

	/*
	 * Store cpu number in limit so that it can be loaded
	 * quickly in user space in vgetcpu. (12 bits for the CPU
	 * and 8 bits for the node)
	 */
	d.limit0 = cpu | ((node & 0xf) << 12);
	d.limit = node >> 4;
	d.type = 5;		/* RO data, expand down, accessed */
	d.dpl = 3;		/* Visible to user code */
	d.s = 1;		/* Not a system segment */
	d.p = 1;		/* Present */
	d.d = 1;		/* 32-bit */

	write_gdt_entry(get_cpu_gdt_table(cpu), GDT_ENTRY_PER_CPU, &d, DESCTYPE_S);
}

static int vgetcpu_online(unsigned int cpu)
{
	return smp_call_function_single(cpu, vgetcpu_cpu_init, NULL, 1);
}

static __init int add_vdso_pages_to_page_cache(
		const struct vdso_image *vdso_image, struct inode *inode)
{
	unsigned long i;
	int ret;

	for (i = 0; i < (vdso_image->size / PAGE_SIZE); i++) {
		struct page *page = virt_to_page(vdso_image->data +
						(i << PAGE_SHIFT));
		int ret;

		__SetPageLocked(page);
		ret = add_to_page_cache_locked(page, inode->i_mapping,
						i, __GFP_REPEAT);
		__SetPageUptodate(page);
		__ClearPageLocked(page);
		if (unlikely(ret))
			goto put_pages;
	}
	return 0;

put_pages:
	while (i > 0)
		put_page(virt_to_page(vdso_image->data + (i << PAGE_SHIFT)));
	return ret;
}

static char *vdso_vma_name(struct dentry *dentry, char *buffer, int buflen)
{
	return "[vdso]";
}
static const struct dentry_operations vdso_dops = {
	.d_dname = vdso_vma_name,
};

static __init struct file *init_vdso_file(const struct vdso_image *vdso_image,
					const char *name)
{
	struct super_block *sb;
	struct qstr name_str;
	struct inode *inode;
	struct path path;
	struct file *res;

	if (IS_ERR(vdso_mnt))
		return ERR_CAST(vdso_mnt);
	sb = vdso_mnt->mnt_sb;

	name_str.hash = 0;
	name_str.len = strlen(name);
	name_str.name = name;

	res = ERR_PTR(-ENOMEM);
	path.mnt = mntget(vdso_mnt);
	path.dentry = d_alloc_pseudo(sb, &name_str);
	if (!path.dentry)
		goto put_path;
	d_set_d_op(path.dentry, &vdso_dops);

	res = ERR_PTR(-ENOSPC);
	inode = ramfs_get_inode(sb, NULL, S_IFREG | S_IRUGO | S_IXUGO, 0);
	if (!inode)
		goto put_path;

	inode->i_flags |= S_PRIVATE;
	d_instantiate(path.dentry, inode);
	inode->i_size = vdso_image->size;

	res = ERR_PTR(add_vdso_pages_to_page_cache(vdso_image, inode));
	if (IS_ERR(res))
		goto put_path;

	res = alloc_file(&path, FMODE_READ, &ramfs_file_operations);
	if (!IS_ERR(res))
		return res;

put_path:
	path_put(&path);
	return res;
}


static struct file_system_type vdso_fs_type = {
	.name		= "vdsofs",
	.mount		= ramfs_mount,
	.kill_sb	= kill_litter_super,
};

static int __init init_vdso_fs(void)
{
	int ret;

	ret = register_filesystem(&vdso_fs_type);
	if (ret)
		return ret;

	vdso_mnt = kern_mount(&vdso_fs_type);
	if (IS_ERR(vdso_mnt))
		return PTR_ERR(vdso_mnt);
	return 0;
}

/* XXX: replace BUG_ON with return to old-way vdso handling */
static int __init init_vdso(void)
{
	init_vdso_image(&vdso_image_64);

#ifdef CONFIG_X86_X32_ABI
	init_vdso_image(&vdso_image_x32);
#endif

	BUG_ON(init_vdso_fs());

	vdso_file_64 = init_vdso_file(&vdso_image_64, "vdso_image_64");
	BUG_ON(IS_ERR(vdso_file_64));

	/* notifier priority > KVM */
	return cpuhp_setup_state(CPUHP_AP_X86_VDSO_VMA_ONLINE,
				 "AP_X86_VDSO_VMA_ONLINE", vgetcpu_online, NULL);
}
subsys_initcall(init_vdso);
#endif /* CONFIG_X86_64 */
