# 65 / ARC 1 — skeleton boots (2026-07-06) — GATE PASSED

## What landed
- **dolphin-chassis/** forked from `dolphin/` at `1ccbcaa` (local git clone; submodules fetched
  shallow minus Qt/FFmpeg-bin; provenance in `CHASSIS.md`). Builds `dolphin-emu-nogui` with
  `-DENABLE_QT=OFF` (no Qt on this Mac; PlatformMacos gives a real Metal window).
- **`CPUCore::StaticRecomp` (=6)** end to end: enum + `AvailableCPUCores` + `InitJitCore` case;
  `StaticRecompCore : JitBase` (`Source/Core/Core/PowerPC/StaticRecomp/`): CachedInterpreter-shaped
  Run loop, D1 full sync at native-burst boundaries (gpr/ps/cr/xer/fpscr/msr/sprs/sr/gqr/reserve),
  interpreter fallback via `SingleStepInner()`, empty JitBaseBlockCache subclass, D3 fixed 64-cycle
  dispatch cost.
- **Module ABI v1** (`StaticRecompABI.h` + vendored DolRecomp CPUState @ GXRuntime `fb103f1`):
  desc = {abi_version, cpu_abi_version, cpu_state_size, game_id, entry, dispatch, on_state_loaded,
  code_ranges, smc_ranges}; export `staticrecomp_get_module`. Loader validates all, refuses →
  interpreter-only. Autoload `<UserDir>/StaticRecompModules/g<GAMEID>_recomp.dylib`, env override
  `STATICRECOMP_MODULE`. (Deviation from D5's literal addr→fn table: generated code is chunk-dispatch
  shaped, so the module exports dispatch()+coverage ranges instead — same semantics.)
- **Hooks v1:** external_read/write → `mmu.Read<T>/Write<T>` (with lazy guest-MSR propagation +
  MSRUpdated); external_pointer → Dolphin L1 cache for 0xE0000000 (locked cache); instruction_fallback
  → interpreter one-step; host_call/read32/write32 = NULL. SMC ranges plumbed through ABI, not yet
  enforced (D4 = arc 2 by plan).
- **StrikersRecomp/chassis-module/** → `gG4QE01_recomp.dylib` (163 chunks + GXRuntime cpu.c +
  glue; ranges regenerated from generated.h/generated_smc.txt at build time).

## Gate evidence
- **Strikers (module + CPUCore=6):** boots Health&Safety → title → FE menus → team select →
  "Today at the Palace" VS screen (framedumps read visually) → **into the live match** (user
  observed on screen), full speed (~60 dumped frames/sec, 2419 frames/55s at boot).
- **Native counter:** `[staticrecomp] native=998244353 fallback=4009125` (99.6% native) after
  reaching gameplay; lldb `chassis_dispatch` breakpoint trace independently showed guest PCs
  dispatching natively.
- **Second ISO invariant:** Melee, chassis build, default core (JITARM64), no module — boots to
  its auto demo match ("Ready", Pokémon Stadium) at full speed, 1543 frames/45s; zero StaticRecomp
  involvement. Stock-identical behavior. (Baseline pre-fork-changes build also verified booting
  Strikers to Health&Safety.)

## Two boot bugs found+fixed (details in KNOWLEDGE/dolphin-chassis.md)
1. Frozen guest timebase: `ReadFullTimeBaseValue()`/spr[TL] is a stale lazy cache — must use
   `SystemTimers::GetFakeTimeBase()`. Symptom: hang spinning `__OSInitAudioSystem` ↔ `OSGetTick`.
2. Native bursts breaking on ANY pending exception: masked EXT_INT (EE=0) is unclearable by
   `CheckExceptions` → one-dispatch bursts + sync thrash. Fix: break only on synchronous bits;
   external ints delivered at slice start by Advance, stock-identically.

## Surprises / cautions
- The reported "in-game freeze + stutters then crash" was **lldb-inflicted** (EXC_BREAKPOINT in
  chassis_dispatch from a leftover trap after SIGTERM'ing an attached lldb; per-hit trapping =
  stutter). Verify any future freeze WITHOUT a debugger attached first.
- Framedump runs fill the disk (~GB/min); ENOSPC bricked the harness mid-session. Clean Dump/Frames.
- Dolphin file logging (`WriteToFile`) never materialized in the nogui agent shell; console logging
  via `<user>/Config/Logger.ini` works.

## In-game observations (user, live window, end of arc 1)
- **Strikers in-match: ball nowhere, no kickoff sequence, goalie completely missing, camera stuck
  bottom-left.** This is the session-57 lazy-FPU collapse class (recomp-codegen.md §Lazy FPU), with
  *different* presentation because the chassis has NO Strikers host FPU-cache workaround (that was
  StrikersRecomp host HLE; chassis host_call=NULL, so the raw MSR[FP] defect shows). Confirms
  generated FP-unavailable semantics as arc 2's #1 correctness item.
- **FPS low/stuttery in BOTH Strikers and Melee.** Melee ran stock JITARM64 with no chassis code in
  the loop → NOT a StaticRecomp regression. Prime suspect: framedump capture was ON in every watched
  run (DumpFrames=True ≈ 60 PNG writes/sec). First arc-2 action: one clean run per game with dumps
  OFF before treating perf as real chassis work.

## Exact next step (ARC 2 — memory+hooks complete, coverage up)
Per 00_PROGRAM.md: all 7 hooks bound (incl. read32/write32 rid pair), paired-single/GQR quantized
load/store diffed vs Dolphin tables, gather-pipe fast path via `ppc_state.gather_pipe_ptr`, D4 SMC
enforcement + session demotion guard, dcbz/dcbf semantics via MMU helpers. Gate: gameplay >90%
native dispatches (already 99.6% at boot→match — measure in-match), zero unexpected demotions,
invariant re-check. **Arc-1-observed demand signals to burn down in arc 2:** black THP intro movie
(LC path), menu stutters (verify clean first — may need gather-pipe fast path), lazy-FPU MSR[FP]
generated semantics (recomp-codegen.md remedy; needed for gameplay correctness / arc-4 lockstep).
