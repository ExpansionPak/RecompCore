# 65 / ARC 4 — lockstep differential + game matrix (IN PROGRESS; RESUME AT 4.5)

**Status: arc 4 NOT gate-passed.** The FP/paired-single/load-store correctness
foundation (4.3/4.4) landed and every offline suite is green; the lockstep
harness itself (4.5), the divergence burndown (4.6), the module-less game
matrix (4.7), and fps/coverage numbers (4.8) are NOT done. Wound down early on
a hard context blocker. Next session RESUMES arc 4 at task 4.5.

## What landed this session (4.3 + 4.4 — the correctness foundation)

The whole DolRecomp FP unit was rewritten from ad-hoc inline C into a set of
**shared helpers in `cpu.c` that mirror Dolphin's interpreter bit-for-bit**, and
the emitter now emits *calls* to them. This is the "port the WHOLE unit"
objective: scalar single/double, the FMA family, estimates, frsp, fctiw, fcmp,
every ps_* form, FP loads/stores, string stores, and the FPSCR control writes —
not just the shapes the old fix_generated regexes happened to catch.

### DolRecomp `src/core/cpu.c` (+ `cpu.h` prototypes) — new Dolphin-exact unit
- **NI_* arithmetic** (`ni_add/sub/mul/div`): PPC NaN propagation in a→b→c
  order, `make_quiet`, VXISI/VXIMZ/VXIDI/VXZDZ/VXSNAN/ZX exception bits,
  ClearFIFR on inf/NaN. Returns `{value, exception}`.
- **`ni_madd_msub`**: single-precision path does `Force25Bit(c)` then a 64-bit
  fused `fma`, then the **round-once even-tie fix** via Ole Møller 2Sum error
  term (Dolphin's algorithm verbatim — the Mario Strikers `0x42480000 *
  0xbc88cc38 + 0x1b1c72a0` case that rounds differently than f32(fma(f64…))).
- **`force_single`**: on arm64/x86 Dolphin arms hardware FTZ when FPSCR.NI, so
  the software part is just the pre-rounding subnormal-single flush quirk; writes
  BOTH PS lanes (Dolphin `Fill`). `fp_write_single/double` centralize the FPRF
  classify + lane policy (single→both lanes+classify_f32; double→PS0+classify_f64).
- **VE/ZE write gating** (`fp_invalid_gated`): matches Dolphin's `if (VE==0 ||
  HasNoInvalidExceptions())` guard on every op's write-back.
- **Ops**: `ppc_fadds/fsubs/fmuls/fdivs/fadd/fsub/fmul/fdiv/fmadd_op(single,
  subtract,negative)/frsp/fres_op/frsqrte_op/fctiw_op/fcmp(ordered)`, and
  `ppc_ps_add/sub/mul/div_op`, `ppc_ps_madd_op`, `ppc_ps_madds0/1`,
  `ppc_ps_sum0/1`, `ppc_ps_muls0/1`, `ppc_ps_res/rsqrte_op`.
- **FP loads/stores** `ppc_lfs/lfd/stfs/stfd_op`: word-alignment exception +
  `ConvertToDouble`/`ConvertToSingle` bit repack (PEM algorithm, preserves
  SNaN/subnormal exactly — NOT `(f64)(f32)`). Return false on exception so the
  emitter skips update-form RA write-back.
- **`ppc_lwarx_op`/`ppc_stwcx_op`**: alignment + exact `reserve_address` match;
  CR0 = EQ|SO / SO. **Removed `clear_matching_reservation`** — Dolphin's
  single-core model never clears a reservation on an ordinary store (the old
  cache-line clear made stwcx fail where Dolphin succeeds).
- **`ppc_stsw`**: mirrors Dolphin `Helper_StoreString` (aligned 32-bit
  read-modify-write of the misaligned head/tail words, LE→alignment exception).
- **`ppc_fpscr_control_updated` + `ppc_arm_host_fp_mode`**: mtfsf/mtfsb/mtfsfi/
  mcrfs recompute VX/FEX and **re-arm the host FPCR (arm64) / MXCSR (x86)
  rounding+FTZ from RN/NI**, mirroring Dolphin FPSCRUpdated→RoundingModeUpdated→
  SetSIMDMode. `ppc_mtfsb0/1_op` (mtfsb1 routes exception bits through the
  FX-raising `set_fp_exception`). FPSCR bit macros + `clear_fifr` added.
- **psq semantics fixed to Dolphin** (catalogued arc-3 deferrals, now closed):
  non-indexed psq checks **only HID2.LSQE** (never PSE); **no alignment
  exceptions** (Helper_De/Quantize read/write unaligned); **invalid GQR types
  1-3 → 0.0 both lanes on load / nothing on store**; type-0 uses
  ConvertToDouble/ConvertToSingleFTZ; **NaN quantizes to 0** (arm64
  `SType(clamp(NaN))`).

### DolRecomp `src/backend/emitter.c`
All FP/ps/load-store cases now emit helper calls. Notable: `fmr` is **PS0-only**
(Dolphin `fmrx` = `SetPS0`, does NOT touch ps1); `ps_merge*`/`ps_sel` are raw
f64 lane moves via temporaries (correct rD==rA/rB aliasing); `fsel`/`ps_sel` use
`>= -0.0`. fcmp/ps_cmp → `ppc_fcmp`. stswi/stswx → `ppc_stsw`.

### `fix_generated.py` — deleted ALL FP rules
The emitter is now correct at source, so SINGLE_BINARY / PS_ROUND / LFS_ASSIGN /
FMR_ASSIGN / PS_FMA_INIT / PS_FMA_ACC are gone. **FMR_ASSIGN was actively WRONG**
once semantics were pinned: it mirrored fmr to ps1, but Dolphin's fmr is
PS0-only. Only the constant-time chunk dispatch transform remains. Regenerated
chunks: 163, fix_generated idempotent (1 dispatch correction), **zero** leftover
`(f32)ctx->` arithmetic.

### Evidence (all green)
- **Oracle `host_diff`**: 239 cases, **0 unexpected, 0 XPASS**, 6 XFAIL — and
  those 6 are a **test-harness artifact, not an emitter bug** (see below). 93
  real FP/frsp/FPSCR mismatches retired as genuine fixes.
- **`dedicated_diff`**: 26 cases, 0 unexpected. **`emitted_diff`**: 151 PASS, 0
  XFAIL, 0 XPASS (9 retired). **DolRecomp ctest: 10/10.**
- **`pc_reference`**: 2 stale psq expectations updated to Dolphin behavior
  (psq_st NaN→0; psq_l unaligned allowed — both were arc-3-catalogued deltas).
- **GXRuntime**: cpu.c re-mirrored from DolRecomp + the `external_pointer`
  field (psq raw-pointer fast paths DROPPED — a divergence risk that used
  `(f64)f32_value` ≠ ConvertToDouble; perf is arc-5, re-addable Dolphin-exact).
  cpu.h: new prototypes + `bool` psq. runtime_tests psq case rewired to
  `external_read`/`external_write` (the real runtime backs 0xE0000000 on the
  mmio bus via external_read, NOT a raw pointer; write-count asserts 0→2).
  **GXRuntime ctest: 16/16.**
- Strikers module + dolphin-chassis nogui both **link clean**.

## The stsw "failure" the USER correctly flagged as a test flaw
`host_diff` shows 6 XFAIL on `auto.stswi/stswx mem[133..135]`. Root cause: the
auto case is `stswi r12, r13, 17` with **EA = gpr[13] = 0x80065EE0**, an
oracle-window pointer. The host harness's `normalize_pointer()` remaps that
register to `0x80000080` so the EA lands in the 512-byte test RAM window — but
stswi *also stores gpr[13]'s value as its 2nd word*, so the emitted code
faithfully stores the remapped `0x80000080` while the golden captured the
original `0x80065EE0`. **`ppc_stsw` is proven byte-exact** to Dolphin on the
un-remapped inputs (isolated test → `80 06 5E E0`). Catalogued as a harness
artifact in `host_diff_run.c` with the proper fix (regenerate the capture so
store-string data registers don't alias a remapped EA pointer). NOT an
emitter/runtime defect.

## Exact next step — RESUME ARC 4 at task 4.5
1. **First: ground the FP rewrite in the real renderer.** Nothing has been run
   in-game yet this session — the FP unit is validated only offline (oracle +
   GXRuntime tests). Run the chassis with the new module (recipe in
   01_RESUME_PROMPT / dolphin-chassis.md) and confirm a match still plays
   correctly + baselines hold (99.88% native, ~60fps). If anything regressed,
   the FP rewrite is the suspect — bisect with CPUCore=1 control.
2. **Build the lockstep harness (4.5):** `STATICRECOMP_LOCKSTEP=1` in
   StaticRecompCore::Run — per native dispatch, snapshot PowerPCState, run
   native + SyncOut, restore snapshot, `SingleStepInner` the interpreter to the
   same end PC, compare gpr/fpr(ps0+ps1)/cr/xer/lr/ctr/msr/pc (+srr on exception)
   and memory writes; dump entry pc + diff fields on divergence. Windowed
   (start/limit env), dedupe by entry pc, zero behavior change when off. The
   interpreter and native now share an identical FP model, so divergences should
   be rare and load-bearing.
3. 4.6 burn down over boot→match; 4.7 Melee + oracle DOL module-less; 4.8
   fps/coverage vs CPUCore=1.

## Surprises / method notes
- The FP rewrite retired 93 oracle XFAILs + 9 emitted_diff XFAILs in one pass —
  strong evidence the shared-helper approach is right. The remaining 6 are the
  harness artifact above.
- `fix_generated`'s FMR→ps1 mirror was a latent correctness bug (Dolphin fmr is
  PS0-only). Deleting the regex pile in favor of a correct emitter removed it.
- macOS `make` in tests/oracle does NOT depend on emitter.c/cpu.c — after
  editing those, `rm host_diff_gen host_generated.c host_diff dedicated_*` then
  `make diff dedicated`, or the diff runs against stale emitted code.
- Task list was cleared mid-session (parallel-session interference, failure mode
  #6) — the ledger checkpoint is authoritative.
- cwd drifted twice (failure mode #7): a `grep`/`ninja` after a `cd`-less
  compound and the regen `cp` block. Absolute paths everywhere.
