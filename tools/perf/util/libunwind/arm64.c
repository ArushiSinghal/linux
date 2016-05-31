#define REMOTE_UNWIND_LIBUNWIND

#define LIBUNWIND__ARCH_REG_ID libunwind__arm64_reg_id

#include "unwind.h"
#include "debug.h"
#include "libunwind-aarch64.h"
#include <../../../../arch/arm64/include/uapi/asm/perf_regs.h>
#include "../../arch/arm64/util/unwind-libunwind.c"

#undef NO_LIBUNWIND_DEBUG_FRAME
#ifdef NO_LIBUNWIND_DEBUG_FRAME_AARCH64
#define NO_LIBUNWIND_DEBUG_FRAME
#endif
#include "util/unwind-libunwind-local.c"

struct unwind_libunwind_ops *
arm64_unwind_libunwind_ops = &_unwind_libunwind_ops;
