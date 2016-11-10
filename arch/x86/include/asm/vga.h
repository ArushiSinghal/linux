/*
 *	Access to VGA videoram
 *
 *	(c) 1998 Martin Mares <mj@ucw.cz>
 */

#ifndef _ASM_X86_VGA_H
#define _ASM_X86_VGA_H

#include <asm/mem_encrypt.h>

/*
 *	On the PC, we can just recalculate addresses and then
 *	access the videoram directly without any black magic.
 *	To support memory encryption however, we need to access
 *	the videoram as un-encrypted memory.
 */

#ifdef CONFIG_AMD_MEM_ENCRYPT
#define VGA_MAP_MEM(x, s)					\
({								\
	unsigned long start = (unsigned long)phys_to_virt(x);	\
	sme_set_mem_unenc((void *)start, s);			\
	start;							\
})
#else
#define VGA_MAP_MEM(x, s) (unsigned long)phys_to_virt(x)
#endif

#define vga_readb(x) (*(x))
#define vga_writeb(x, y) (*(y) = (x))

#endif /* _ASM_X86_VGA_H */
