# 65 / ARC 3 — timing/interrupts/exceptions: honest per-block downcount (2026-07-06)

## Gate evidence
- **Full match at correct speed, VI-paced:** user played a real match on the final build —
  **locked 100% speed / 100% max / 60 FPS = 60 VPS** ("much better"). Menus measure 99%
  (tb-rate 40.3M/s over a 20s window). Residual: occasional frame stutters/drops — menus
  run with ~zero host headroom; dispatch-perf pass is the arc-5 item.
- **Savestate round-trip:** nogui has NO hotkey scheduler (Qt-only) → added SIGUSR1/USR2
  slot-1 save/load triggers to the fork's nogui platform loop. Round-trip verified live:
  save → 15s of play → load → screen jumps back and continues correctly; on load,
  JitInterface::DoState → ClearCache resets every chunk to Unverified, lazily re-verifies,
  and the VMBASE SMC chunk re-retires (new SMC stderr line right after the load = the D4
  guard riding savestates exactly as designed).
- **G008 differential suites green at final state:** oracle full-state 239 cases / 99
  catalogued XFAIL / 0 unexpected / 0 XPASS; dedicated 26 cases 0 unexpected; DolRecomp
  ctest 10/10; GXRuntime 16/16 (with new ABI-v2 static asserts).
- **Exceptions/rfi exercised:** the user-played session closed at **99.91% native**
  (26.38B native / 23.9M interpreter) with **native_exc=789,607** — FP-unavailable/sc/trap
  raised and delivered by generated code, handler runs, rfi re-enters mid-function via the
  PC switch (a correct lazy-FP match is impossible without it). smc_failed=1 steady.
- **PRIME INVARIANT re-check:** Melee, chassis binary, no module, default core → 1375
  frames rendered, ZERO `[staticrecomp]` lines (StaticRecomp core never constructed).
- **Standalone StrikersRecomp post-regen:** boots with auto-input through
  InitializeGameState into attract (1759 scripted frames), 0 unhandled-instruction
  warnings; user: "It ran fine."

## What landed
1. **Generated-code ABI v2: per-block downcount emission** (the arc's core item).
   Emitter partitions each function into accounting blocks (leaders = entry, local branch
   targets, successors of control transfers) and emits `ctx->downcount -= n` at each block
   top; n mirrors Dolphin PPCTables num_cycles per opcode (div 40, mul 5, lmw/stmw 11,
   fdiv 31/17s, dcbz 5, sync/mtfs* 3, sc/rfi/mtspr 2, default 1). Fallback-routed
   instructions cost 0 (environment charges them: SingleStepInner, or the chassis cache-op
   fast path at icbi 4 / dcbf/dcbst/dcbi 5). `downcount` = tail s64 charge accumulator in
   CPUState; GXRUNTIME_CPU_ABI_VERSION 1→2 in all three header copies (DolRecomp,
   GXRuntime, chassis vendored); module load rejects on mismatch.
2. **Chassis consumes charges per dispatch:** burst loop flushes ctx→ppc.downcount after
   every dispatch (min charge 1 → a zero-charge dispatch can't spin the burst); SyncOut
   flushes too (fallback path). CYCLES_PER_DISPATCH deleted. The per-dispatch flush IS the
   dispatcher back-edge check: bursts break on downcount with ≥ CachedInterpreter
   frequency → EXT_INT latency stock-identical by construction (measured nothing worse;
   no per-N-branches emission needed).
3. **Savestate hooks:** ClearCache override (chunks → Unverified, failed count reset) —
   fires on savestate load; SIGUSR1/SIGUSR2 savestate triggers in nogui Platform
   (handlers set atomic flags; platform loop calls State::Save/Load(slot 1)).
4. **Diagnostics:** `bursts=`/`charged=` in verbose counters (charged/Δwall ÷ 486M =
   speed without a screenshot; charged/native = avg block cost; native/bursts = burst
   length); build stamp (__DATE__ __TIME__) on core-init + module-load stderr lines.
5. **SingleStep()** via interpreter was already correct (arc-1 code) — unchanged.

## The perf incident (half the arc's budget — read this)
First run with honest charging read 25% speed / 15 FPS and looked like "downcount is a
trap". It was three stacked things (post-hoc per-10s tb-rate decomposition of all runs):
- **The 25% run itself was starved by a parallel `cmake -j8` standalone build I had
  launched alongside it** (its module had loaded fine, 99.9% native). Never benchmark
  with a build running.
- **Flat-6 had been overcharging tiny blocks and undercharging big ones; menus really ran
  ~52-60% all along** (idle-spin blocks cost ~3-4, not 6) and nobody had measured menus.
  Honest charging exposed the real dispatch-rate ceiling.
- **A per-dispatch `GetFakeTimeBase()`** (added mid-arc by a parallel session) measured
  **~34% of the entire CPU thread** in `sample` (~80M calls/s). Removed: ctx->timebase
  refreshes at SyncIn only; mid-burst staleness ≤ one slice (~20k cycles) and a time-poll
  loop still drains the slice via its own charges. Menus 60%→99%, match locked 100%.
- A parallel session also tried a per-function `u32 cycles` accumulator + 512-inst chunks
  to fix it (reverted): a local live across the label web + PC switch makes clang compile
  the whole 4096-case chunk as ONE loop — 8KB frames, spills, ~20x. Block charges as
  direct `ctx->downcount -= n` statements keep functions DAG-shaped and cost ~nothing
  (chunk_0149 -O2: 80B frame with charges vs 144B without). Full writeup:
  KNOWLEDGE/recomp-codegen.md §Guest-work accounting; trap list: 01_RESUME_PROMPT.md.

## Surprises / method notes
- Session start found the generated C MISSING from disk (empty chunks/) and a stale task
  list claiming arc-3 items complete — trust only git + working tree (now failure mode #6).
- Relative paths bit twice more (dead launch; ledger line written into
  StrikersRecomp/runtimeharness/) — absolute paths in EVERY call, including appends.
- Foreign 512-chunk regen contaminated generated/chunks (1463 files, module silently
  linked 1304 objects); chunk-count != 163 ⇒ rm + regen + RE-CONFIGURE (GLOB is
  configure-time).
- The counters + `sample` replaced screenshots for perf: Δcharged/Δtb ≈ 12 proves honest
  charging; user overlay readings matched computed speeds throughout.

## Exact next step (ARC 4 — lockstep differential + game matrix, per 00_PROGRAM.md)
Debug mode: per dispatch snapshot → native → interpreter re-run → compare gpr/fpr/cr/pc/
mem-writes; burn down ALL divergences over Strikers boot→match (FPSCR/psq edge catalog in
recomp-codegen.md is the hit list). Run ≥2 more ISOs module-less for the invariant.
Measure fps vs stock JIT + coverage%. Known perf residual for arc 5: frame stutters, menus
at zero headroom (Run-loop body ≈17% of thread, chassis_dispatch subtree ≈47%).
