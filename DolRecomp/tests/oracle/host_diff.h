#ifndef DOLRECOMP_ORACLE_HOST_DIFF_H
#define DOLRECOMP_ORACLE_HOST_DIFF_H

#include "core/types.h"

#define HOST_ORACLE_MAX_WORDS 16u
#define HOST_ORACLE_MEM_SIZE 256u

typedef struct {
    const char* name;
    u32 raw[HOST_ORACLE_MAX_WORDS];
    u32 raw_count;
    u32 address;
    u32 guest_mem_base;
    u32 oracle_mem_base;
    u32 in_gpr[32];
    u64 in_fpr[32];
    u64 in_ps1[32];
    u32 in_cr;
    u32 in_xer;
    u32 in_lr;
    u32 in_ctr;
    u32 in_msr;
    u32 in_fpscr;
    u32 in_srr0;
    u32 in_srr1;
    u32 in_hid2;
    u32 in_sr[16];
    u32 in_gqr[8];
    u8 in_mem[HOST_ORACLE_MEM_SIZE];
    u32 out_gpr[32];
    u64 out_fpr[32];
    u64 out_ps1[32];
    u32 out_cr;
    u32 out_xer;
    u32 out_lr;
    u32 out_ctr;
    u32 out_msr;
    u32 out_fpscr;
    u32 out_srr0;
    u32 out_srr1;
    u32 out_dar;
    u32 out_dsisr;
    u32 out_ear;
    u32 out_hid2;
    u32 out_sr[16];
    u32 out_gqr[8];
    u32 out_exception;
    u8 out_mem[HOST_ORACLE_MEM_SIZE];
} HostOracleCase;

#endif
