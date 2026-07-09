// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    return dolrecomp_call(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    .abi_version = STATICRECOMP_ABI_VERSION,
    .cpu_abi_version = DOLRUNTIME_CPU_ABI_VERSION,
    .cpu_state_size = (u32)sizeof(CPUState),
    .game_id = MODULE_GAME_ID,
    .entry_point = DOLRECOMP_ENTRY_POINT,
    .dispatch = chassis_dispatch,
    .on_state_loaded = chassis_on_state_loaded,
    .code_ranges = s_code_ranges,
    .num_code_ranges = (u32)(sizeof(s_code_ranges) / sizeof(s_code_ranges[0])),
    .smc_ranges = s_smc_ranges,
    .num_smc_ranges = (u32)(sizeof(s_smc_ranges) / sizeof(s_smc_ranges[0])),
    .chunk_ranges = s_chunk_ranges,
    .num_chunk_ranges = (u32)(sizeof(s_chunk_ranges) / sizeof(s_chunk_ranges[0])),
    .chunk_hashes = s_chunk_hashes,
};

__attribute__((visibility("default"))) const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    return &s_desc;
}
