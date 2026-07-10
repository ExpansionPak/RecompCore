// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_EMITTER_PRIVATE_H
#define DOLRECOMP_EMITTER_PRIVATE_H

#include "emitter.h"

u32 cr_field_shift(u8 crf);
u32 ppc_mask32(u8 mb, u8 me);
void emit_set_cr0_from_gpr(FILE* out, u8 reg);
void emit_set_cr1_from_fpscr(FILE* out);
void emit_compare_s32(FILE* out, u8 crf, const char* lhs, const char* rhs);
void emit_compare_u32(FILE* out, u8 crf, const char* lhs, const char* rhs);
void emit_fcompare(FILE* out, const PPCInst* inst);
void emit_dform_ea(FILE* out, u8 ra, s16 simm, bool update);
void emit_xform_ea(FILE* out, u8 ra, u8 rb, bool update);
void emit_load(FILE* out, const PPCInst* inst, const char* read_expr, bool update);
void emit_loadx(FILE* out, const PPCInst* inst, const char* read_expr, bool update);
void emit_store(FILE* out, const PPCInst* inst, const char* write_func, const char* cast_type, bool update);
void emit_storex(FILE* out, const PPCInst* inst, const char* write_func, const char* cast_type, bool update);
void emit_fload(FILE* out, const PPCInst* inst, bool single, bool update);
void emit_floadx(FILE* out, const PPCInst* inst, bool single, bool update);
void emit_fstore(FILE* out, const PPCInst* inst, bool single, bool update);
void emit_fstorex(FILE* out, const PPCInst* inst, bool single, bool update);
void emit_psq_load(FILE* out, const PPCInst* inst, bool indexed, bool update);
void emit_psq_store(FILE* out, const PPCInst* inst, bool indexed, bool update);
void emit_dcbz(FILE* out, const PPCInst* inst);
void emit_branch_condition(FILE* out, u8 bo, u8 bi);
bool branch_target_is_local(u32 func_start, u32 func_end, u32 target);
void emit_direct_branch(FILE* out, const PPCInst* inst, bool local_target);

// Emitter split dispatch functions
bool emit_float_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end);
bool emit_integer_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end);
bool emit_branch_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end);
bool emit_system_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end);

#endif /* DOLRECOMP_EMITTER_PRIVATE_H */
