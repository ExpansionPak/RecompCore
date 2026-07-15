# 65 / ARC 2 — memory+hooks complete, coverage up (2026-07-06) — GATE PASSED

## Gate evidence (final build, user playing a real match)
- **99.88% native dispatches in-match** (25.08B native / 29.5M interpreter cumulative; identical
  99.88% over the last 120s in-match window). Gate bar was >90%.
- **Zero unexpected demotions:** `smc_failed=1` steady — the ONE retired chunk
  [0x8025D6C0,0x802616C0) is genuine SMC (SDK VMBASE patches its own DSI/ISI handlers;
  patcher sites for it are in generated_smc.txt). 322 verifications, 167 reverify events, all cheap.
- **Invariant re-check:** Melee (GALE01), chassis binary, no module → boots to its auto demo match
  (Mario vs Fox, Pokémon Stadium) on stock JITARM64; zero `[staticrecomp]` lines. Stock-identical.
- **Correctness observed live:** match with ball + Kritter goalie + HUD + correct camera
  (arc-1 lazy-FPU collapse gone); THP intro movie plays (user: "works perfectly"); in-game
  Speed 100% / Max 113% / ~60 FPS; heavy cutscenes ~80% speed (documented residual → arc 3/5).
- Standalone StrikersRecomp after regen: boots with auto-input into game-state init, zero
  unhandled-instruction warnings. DolRecomp 10/10 ctest; oracle full-state 239 cases
  0 unexpected/0 XPASS; dedicated 26 cases 0 unexpected; GXRuntime 16/16.

## What landed
1. **Lazy FP as generated semantics (the arc's #1 correctness item):** emitter emits
   `ppc_fp_available` guard before every FPU op (contiguous opcode block, `ppc_op_uses_fpu`);
   FP-unavailable exception (0x800) added to both cpu.c copies. Chassis needs no MSR.FP gate —
   a coarse gate was tried and REJECTED with evidence (idle thread runs FP-off → hottest loop
   interpreted). Standalone opts out via `ppc_lazy_fp_set_enabled(false)` (keeps host FPU cache).
2. **Environment-instruction deferrals in the emitter:** unmodeled mtspr/mfspr (DMAU/DMAL, DEC,
   HID0, WPAR, SPRGs, BATs, PMCs) + dcbf/dcbst/dcbi/icbi → `ppc_fallback_instruction`. Fixed the
   black THP movie (Dolphin's mtspr DMAL does the real LC↔RAM DMA) AND silent SPRG/DEC/BAT drops.
   Standalone fallback hook upgraded to preserve its historical semantics; dedicated_diff harness
   binds an env fallback; codegen_compile now keys on the "unknown instruction fallback" marker.
3. **D4 SMC guard = verify-on-entry (design corrected by evidence):** demote-on-invalidate demoted
   ALL 163 chunks during DOL load (Dolphin invalidates icache while loading code). Now: module
   carries per-chunk FNV-1a-64 of original DOL text (ABI v2: chunk_ranges + chunk_hashes;
   gen_module_tables.py parses main.dol); chunks verify on first entry, invalidation → Unverified
   (re-verify on next entry), mismatch → Failed until next invalidation. JitCache
   InvalidateICacheInternal made virtual; EmptyBlockCache override feeds the guard.
4. **All 7 hooks bound:** external_read32/write32 (eciwx/ecowx → MMU, mirroring Dolphin),
   host_call (explicit no-HLE false), external_pointer kept L1C-only with documented rationale
   (EA→RAM mapping is MSR/BAT-dependent; MMU is the only correct authority — GetPointerForRange
   rejected).
5. **Gather-pipe fast path:** 0xCC008000-page stores → GPFifo::Write* directly (JIT
   optimizeGatherPipe precedent; GPFifo maintains ppc_state.gather_pipe_ptr).
6. **psq/GQR diff vs Dolphin (arc item):** quantize rewritten to mirror Dolphin exactly
   (f32 pre-round → f32 2^scale multiply → f32 clamp → truncate; NaN→0); dequant proven
   equivalent; edge catalog (NaN payloads, invalid types, alignment, PSE/LSQE nuance) recorded in
   recomp-codegen.md for arc-4 lockstep.
7. **D3 recalibration + dispatch perf:** CYCLES_PER_DISPATCH 64→6 (the 64 time-warp made the game
   internally lag-skip to 8 FPS while Speed/VPS read 100%/60 — mechanism documented in
   dolphin-chassis.md); last-chunk-index cache kills the per-dispatch binary search; zero-sync
   fast path for dcbf/dcbst/dcbi/icbi (streaming flush storms no longer pay SyncOut/SyncIn).

## Surprises / method notes
- **Stale-binary trap:** a full debug cycle was lost to running an old chassis binary after
  editing StaticRecompCore (phantom "interpreter dominance" = the un-removed coarse FP gate).
  Rebuild-the-whole-chain recipe now in dolphin-chassis.md.
- **The discriminating-test discipline paid twice:** (a) one instrumented verbose line
  (idx/st/msr per fallback sample) collapsed the whole hypothesis tree and exposed the stale
  binary; (b) CPUCore=1 on the same binary separated core-caused from environment-caused FPS.
- **User collaboration:** the user is at the machine and prefers being ASKED for observations
  (FPS overlay readings, movie rendering, match navigation) over automation loops — recorded in
  tools.md. Their readings (8 FPS + 60 VPS + 100% speed) were the key clue for the D3 time warp.
- Regen output lands in a NESTED `generated/generated/` (title_id defaults to "generated").

## Exact next step (ARC 3 — timing/interrupts/exceptions, per 00_PROGRAM.md)
Per-block downcount emission (DolRecomp; retires the fixed 6-cycle approximation AND the ~80%
cutscene residual); external-interrupt check on dispatcher back-edge; exception delivery = full
sync → Dolphin vectors; rfi/OSLoadContext mid-function re-entry exercised; SingleStep() via
interpreter; savestate full-sync hooks. Gate: full match at correct speed VI-paced; savestate
round-trip mid-match; G008 differential suite green on chassis build.
