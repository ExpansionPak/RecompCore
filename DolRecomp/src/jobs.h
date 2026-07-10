#ifndef DOLRECOMP_JOBS_H
#define DOLRECOMP_JOBS_H

#include "core/types.h"
#include "frontend/decoder.h"

typedef struct {
    const PPCInst* insts;
    u32 count;
    u32 func_addr;
    char path[1200];
    char include_name[512];
} ChunkJob;

int run_chunk_jobs(const ChunkJob* jobs, u32 job_count, u32 requested_jobs);
u32 effective_chunk_jobs(u32 job_count, u32 requested_jobs);

#endif /* DOLRECOMP_JOBS_H */
