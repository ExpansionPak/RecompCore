# Arc 5 — GATE PASSED (product hardening) → PROGRAM 65 arcs 1-5 COMPLETE

**Date:** 2026-07-08 · **Goal:** G016 · **Arc:** 5 · **State:** GATE MET → ARC 5 COMPLETE.
Gate (00_PROGRAM.md): "**distributable chassis + Strikers module; invariant statement verified**" — met.

## What landed (5 gate items)
1. **One-command packaging** — `StrikersRecomp/tools/package_module.sh [USER_DIR]`: builds the module
   (ThinLTO Release) + installs `gG4QE01_recomp.dylib` into `<USER_DIR>/StaticRecompModules/`. Chassis
   autoloads by disc ID. Verified: no env, CPUCore=6 → native>0, smc_failed=1.
2. **Config toggle** — `Main.Core.StaticRecompModule` (bool, default true) in `MainSettings.{h,cpp}`,
   read in `StaticRecompCore::LoadModule` (early return). `-C Dolphin.Core.StaticRecompModule=False`
   forces interpreter-only — the on-demand PRIME-INVARIANT path — and **wins even over an explicit
   `STATICRECOMP_MODULE` env / an autoload module**. Verified: Strikers native=0 + "module disabled by config".
3. **Dispatch perf pass** — see "Perf" below. host_call null + module ThinLTO; scoped sorted-table/
   direct-index item was already satisfied; interpreter-block memo N/A (fallback 0.08%).
4. **Upstream-sync policy** — `CHASSIS.md`: seam-file table (the ~10 upstream files we touch) + rebase
   procedure + re-verify checklist (build, invariant, oracle, lockstep).
5. **KNOWLEDGE** — `dolphin-chassis.md` §Arc-5 (perf-profile truth, LTO caveat, future levers, workflow facts).

## Perf (the honest story)
Profiled a heavy match with `sample`. Dominant cost = **out-of-line helper CALLS inside the dylib**
(`mem_read32` ~9-10%, the FP/paired-single ops `ni_madd_msub`/`ppc_ps_madds`/`ppc_fmadd_op`,
`ppc_psq_load`/`psq_load_value`, `ppc_lfs_op`) + `ppc_host_call` ~2.7% (a per-dispatch cross-dylib
indirect call to a hook that ALWAYS returns false — the chassis does no HLE). Burst-loop body ~19%.

Landed two semantics-preserving wins, both proven bit-exact by lockstep:
- **Null the chassis `host_call`** (removed `HookHostCall`) → `ppc_host_call` 97→**0 samples**.
- **ThinLTO on the module** (`-flto=thin`). VERIFIED applied (.o = LLVM bitcode; `-O3 -flto=thin`).
  **Surprise:** ThinLTO keeps the LARGE thousands-of-callsite helpers (`mem_read32`, FP ops) OUT-OF-LINE
  (flat counts ~unchanged) — it only folds the small leaves (`translate_addr`, `read_be32`, `ni_*`). So
  LTO's speed benefit is modest; kept anyway (correct, zero risk, enables future inlining/cross-chunk opt).

**Measured (overlay, no lockstep, LTO'd module):** heavy jungle-crowd goal-cam **Speed 95% / Max 95%**
(CPU-bound, correct render); kickoff 100%/Max 100%. In line with arc-4's 97% heaviest baseline. Stock
JIT stays Max 379% — that residual ~3.8× is the per-block dispatch MODEL (3 indirect calls/block, no
block-linking), NOT addressable by helper inlining.

## Correctness (two independent proofs)
- **Lockstep**: `STATICRECOMP_LOCKSTEP=1` on the LTO'd module, boot→(user-driven)match →
  **0 DIVERGE over 37.7B native dispatches**. Bit-exact vs Dolphin's interpreter (guards the LTO change
  independently of the oracle, which builds cpu.c without LTO).
- **Render**: live match screenshot correct (Mario 0-1 Luigi, Yoshi dribbling, goalie diving, crowd/HUD/FX).
- **Baselines**: DolRecomp 10/10, GXRuntime 16/16, oracle 239/0 unexpected (6 known stswi/stswx XFAIL),
  dedicated 26/0.

## PRIME INVARIANT re-verified
Module absent ⇒ pure Dolphin interpreter: `native=0`, no exception, on **both** Strikers AND Melee
GALE01. Config-toggle-off is a second invariant path on Strikers itself. (User confirmed Melee renders
fine — slow = expected interpreter, not a hang.)

## Surprises / lessons for next session
- **The user hand-drives to matches — there is no attract-demo to wait for.** A `pc=0x80259294` idle-spin
  in the counters is the idle THREAD between frames, not "stuck at menu": the game can be in an active
  user-driven match while that prints. ASK the user to drive to a named scene (heavy goal-cam vs kickoff);
  fastest instrument, they're watching. (Cost me a wrong assumption this session — they corrected it twice.)
- **Module-less render is correct but SLOW to appear** (pure interpreter); don't misread slowness as a hang.
- **ThinLTO ≠ force-inline.** If you want the memory helpers actually inlined, restructure them (static
  inline fast path) — LTO's importer won't take large hot functions. Verified by `file` on the .o + flat sample.

## Repo state (uncommitted at handoff time — commit is step 5 of ceremony)
- **dolphin-chassis** (branch `arc4-lockstep-zero`): `StaticRecompCore.cpp/.h` (host_call null, remove
  HookHostCall, config-toggle read), `MainSettings.{h,cpp}` (toggle), `CHASSIS.md` (arc-5 + sync policy).
- **StrikersRecomp** (main): `chassis-module/CMakeLists.txt` (+ThinLTO), `tools/package_module.sh` (new).
- **runtimeharness** (main): this handoff, goals.json G016 evidence, KNOWLEDGE/dolphin-chassis.md, ledger.

## NEXT
Program 65 arcs 1-5 all COMPLETE — the Dolphin-chassis path is a distributable product with a verified
invariant. **ARC 6 is a reserved buffer (unspent = win):** optional edge-hardening (FPSCR NI modes, XER
carry edges, dcbz_l, feature_flags) OR a future perf lever if the ~3.8× headroom matters for shipping:
(a) inline flat-RAM memory fast path (~6% recoverable, needs oracle+lockstep re-verify), (b) emitter
block-linking (large, guard hard with lockstep). No forced next step — the program's goal is met.
