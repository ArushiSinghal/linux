#define REMOTE_UNWIND_LIBUNWIND

#define LIBUNWIND__ARCH_REG_ID libunwind__x86_reg_id

#include "unwind.h"
#include "debug.h"
#include "libunwind-x86.h"
#include <../../../../arch/x86/include/uapi/asm/perf_regs.h>

#undef HAVE_ARCH_X86_64_SUPPORT
#include "../../arch/x86/util/unwind-libunwind.c"

#undef NO_LIBUNWIND_DEBUG_FRAME
#define NO_LIBUNWIND_DEBUG_FRAME
#include "util/unwind-libunwind-local.c"

struct unwind_libunwind_ops *
x86_32_unwind_libunwind_ops = &_unwind_libunwind_ops;
