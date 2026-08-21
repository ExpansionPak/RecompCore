// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

/* The x86-64-v3 dispatch path is compiled only when the module was generated
 * with that variant (dolrecomp --targets ... x86-64-v3). moderngekko-port does
 * not pass --targets, so normally it is absent, and referencing it breaks the
 * build two ways on x86-64 with clang or gcc:
 *
 *   module_export.c:23: error: invalid cpu feature string for builtin
 *   module_export.c:24: error: invalid cpu feature string for builtin
 *   module_export.c:35: error: call to undeclared function
 *                              'dolrecomp_call__x86_64_v3'
 *
 * clang does not accept "movbe" or "lzcnt" for __builtin_cpu_supports, and the
 * v3 entry point is neither declared nor defined. Both went unnoticed because
 * the two configurations built regularly fold the whole thing away first: under
 * MSVC and on non-x86-64 targets the guard below is false, so the body is a
 * constant 0 and the call is dead-code eliminated before the compiler sees it. */
#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)
int dolrecomp_call__x86_64_v3(CPUState* ctx, u32 address);

static int host_has_x86_64_v3(void)
{
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static int supported = -1;
    if (supported < 0)
    {
        __builtin_cpu_init();
        supported = __builtin_cpu_supports("avx") &&
            __builtin_cpu_supports("avx2") &&
            __builtin_cpu_supports("fma") &&
            __builtin_cpu_supports("bmi") &&
            __builtin_cpu_supports("bmi2");
    }
    return supported;
#else
    return 0;
#endif
}
#endif

static int selected_dispatch(CPUState* ctx, u32 address)
{
#if defined(DOLRECOMP_MODULE_HAVE_X86_64_V3)
    if (host_has_x86_64_v3())
        return dolrecomp_call__x86_64_v3(ctx, address);
#endif
    return dolrecomp_call(ctx, address);
}

void dolrecomp_indirect_dispatch(CPUState* ctx, u32 address)
{
    (void)selected_dispatch(ctx, address);
}

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    return selected_dispatch(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    return &s_desc;
}
