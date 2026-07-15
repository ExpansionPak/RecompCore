# ARC 6 — JIT-beating dispatch/codegen (design, 2026-07-10)

**Goal (user):** RecompCore runs FASTER than stock JIT on covered code; floor = JIT max speed
(reference: stock JITARM64 Speed 100%/Max 379% vs chassis 95-100%, arc-4 §4.8; baseline re-run
2026-07-10: heavy attract scene chassis cumulative ≈81% speed, consistent).

**Root causes of the ~4x gap (measured/verified in source):**
1. Per-instruction `case`+`label_`+`ctx->pc=` in emitted chunks (emitter.c emit_function): every
   label is switch-reachable ⇒ compiler must keep ALL guest state memory-coherent at every
   instruction ⇒ interpreter-quality codegen (no register allocation).
2. Backward branches + all calls/returns = dispatcher round-trips (`ctx->pc=T; return;`) through
   the chassis burst loop (~19% of thread) + cross-dylib `m_module->dispatch` + 4096-case switch.
3. Out-of-line helper calls for every memory access (mem_read32 ~10%) and every FP-gate
   (`ppc_fp_available` call per FP op) force full spills around each.

**Microbench (scratchpad/microbench/bench.c, M4, clang -O3):** v3 shape (leader-only cases, no
per-inst pc stores, local back-edge budget check, inline mem fast path) = 3.77G guest-inst/s and
60k vs 300M dispatches; asm confirms full register allocation (even auto-vectorized GPR pair).
JIT-at-379% ≈ 1.8G guest-cycles/s ⇒ headroom confirmed.

## Design: CPU ABI v3 + emitter v3 ("hot chunk" model)

**ABI v3 (DolRecomp src/core/cpu.h, mirrored GXRuntime include/core/cpu.h, chassis dolrecomp/cpu.h):**
- `downcount` = env-PRELOADED positive budget. Leaders charge `ctx->downcount -= cost`; emitted
  back-edges/call-sites check `<= 0` and return to dispatcher. Env computes consumed =
  preload − remaining. Hosts that leave it 0 (standalone StrikersRecomp) get per-block dispatch =
  exact v2 behavior (graceful degradation; G011 can adopt budgets later).
  RULE: hosts that install `host_call` (HLE address interception) MUST NOT preload (budget>0 would
  skip HLE on native-chained calls). Chassis: host_call=NULL ⇒ safe.
- New tail field `u32 host_depth` (+version bump to 3): guest-call-depth guard for host-stack
  calls; bl sites stop nesting at DOLRECOMP_MAX_HOST_DEPTH (192) and fall back to dispatcher
  round-trips (unbounded guest recursion cannot overflow the host stack; env resets to 0 per burst).
- `ppc_fp_available` becomes static-inline fast path (lazy-flag + MSR.FP test) with out-of-line
  `ppc_fp_unavailable_exception` slow path; emitter gates only the FIRST FP op per accounting
  block (and after mtmsr) — later ops in the block cannot lose MSR.FP.
- Inline memory fast paths (emitted in generated.h header): dolrecomp_read/write{8,16,32,64} =
  translate(0x8/0xC window) → flat byteswap access, journal-flag + miss → out-of-line cpu.c mem_*.
  cpu.c exports `ppc_mem_journal_active` (plain global, ABI-neutral) so inline writes divert to
  the journaling slow path during lockstep.

**Emitter v3 (DolRecomp src/backend/emitter.c + main.c):**
- `case`/`label_` only at LEADERS (entry + local branch targets + transfer successors — the
  existing accounting-leader set). Non-leader re-entry (exception-retry cia) is env's job:
  chassis interpreter-steps to the next leader (bounded by block length); module exports
  `enterable(pc)` from emitted per-chunk leader bitmaps.
- No per-instruction `ctx->pc=` stores; pc materialized only at dispatcher-return sites.
- Local backward b/bc: `if (ctx->downcount <= 0) { ctx->pc=T; return; } goto label_T;`
- bl/bcl direct: `ctx->lr=R; ctx->pc=T; if (budget<=0 || depth>=MAX) return;
  [if (!CHUNK_OK(idxT)) return;] ++depth; func_TCHUNK(ctx);
  while (ctx->pc != R) { if (budget<=0 || ctx->exception || !dolrecomp_enter(ctx)) { --depth; return; } }
  --depth; /*fallthrough to R*/` — host-stack call with guarded unwind; ANY non-local flow
  (context switch, exception, rfi) unwinds every frame to the dispatcher; the call-loop keeps
  driving natively across indirect flow inside the callee.
- bctrl/bclrl: same shape, dynamic first entry via dolrecomp_enter.
- b non-local: `ctx->pc=T; if (budget>0 && CHUNK_OK) func_TCHUNK(ctx); return;` (sibcall tail).
- bctr: `ctx->pc=target; if (budget>0) dolrecomp_enter(ctx); return;`  blr: `pc=lr; return;`
- Chunk-end fallthrough: tail-call next chunk.
- EXACT cycle accounting: mid-block exception exits refund the unexecuted suffix
  (`ctx->downcount += block_cost − prefix_through_faulting_inst`), faulting instruction charged
  (mirrors Dolphin SingleStepInner returning opinfo cycles on the fault path — verify in source).
  Sites: FP gate, lfs/lfd/stfs/stfd/psq/lwarx/stwcx fail returns, taken tw/twi, eciwx/ecowx.
  This also structurally kills the arc-4 D3 mid-block-entry undercharge (entries = leaders only).
- dolrecomp_call emitted with native indexed dispatch (absorbs fix_generated.py's last rule; keep
  the range-guard text shape gen_module_tables.py parses) + chain loop:
  `while (budget>0 && !exception && enterable(pc)) enter;`
- Leader bitmaps: per-chunk `const u8 dolrecomp_leaders_<addr>[]` in chunk files + table in
  generated.c; `extern const u8* dolrecomp_chunk_states` (NULL ⇒ all-ok; chassis binds its
  m_chunk_state array so cross-chunk native transfers respect SMC demotion + verification:
  transfers into !=VERIFIED chunks return to dispatcher, which verifies/demotes as today).

**Chassis (StaticRecompABI.h v3 + StaticRecompCore.cpp):**
- Desc += `int (*enterable)(u32 pc)`; chassis binds chunk-states via module export
  `dolrecomp_chunk_states` (dlsym) after LoadModule.
- Run loop: SyncIn → preload `m_guest.downcount = ppc.downcount, host_depth=0` → dispatch (module
  chains blocks internally through the slice) → flush consumed → exceptions policy unchanged.
  dispatch rc==0 (non-leader pc) → single interpreter step, retry. DispatchableAt = chunk
  verified && enterable(pc).
- Sync-exception break: hooks that can raise Dolphin-side exceptions (external read/write/32,
  instruction fallback) zero `cpu->downcount` when `ppc.Exceptions & SYNC_MASK` ⇒ module returns
  at the next check (block granularity, same as today). OnICacheInvalidate also zeroes budget
  (SMC demotion takes effect at the next transfer, JIT-equivalent).
- Mid-dispatch flush bookkeeping: m_burst_preload; HookInstructionFallback's SyncOut/SyncIn
  computes consumed = preload − remaining and re-preloads after.
- Lockstep v3: native_charge = consumed (exact); shadow steps interpreter for exactly that many
  cycles then requires pc == end_pc (drop loop-header/undercharge heuristics on the v3 path);
  STEPCAP default raised to cover a slice (~20k steps). Fallback-containing dispatches skipped
  (unchanged). Whole-slice blocks are checked as units (dedup by entry pc unchanged).

**Out of scope (measure first, only if <379%):** inlining Dolphin-exact FP arithmetic helpers;
psq raw-pointer fast paths (arc-4 rejected); block-local pc-rotation to hoist loop-header spills.

## Gates (arc exit)
1. DolRecomp ctest green; oracle host_diff 239/0 unexpected + dedicated 26/0; emitted_diff green.
2. GXRuntime ctest green (cpu/runtime suites).
3. Strikers regen (163 chunks) + module + chassis build; fix_generated v3 no-op.
4. Lockstep boot→match 0 DIVERGE (bit-exactness invariant, the product guarantee).
5. PRIME INVARIANT: module-less arbitrary ISO identical to stock; toggle-off forces interpreter.
6. Standalone StrikersRecomp builds + boots (budget-0 degradation proof).
7. PERF: same-scene A/B vs CPUCore=1 — chassis ≥ JIT Max (379% floor), target >.

Prior state: chassis branch public-main (arc-5 + publication + GXRuntime-naming WIP kept);
DolRecomp switched pr-oracle-suite → main (restore pr-oracle-suite for program 67 when needed).
