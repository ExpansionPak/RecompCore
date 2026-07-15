# PROGRAM 65 — DOLPHIN CHASSIS (ratified 2026-07-06; ACTIVE program)

**Decision (user, 2026-07-06):** commit + branch current state; pursue the Dolphin-chassis path
(S4) as the active program, 4-6 fully-planned arcs. The certificate path (03_GOLDEN_PATH) is
PRESERVED on branch `certificate-path` in GXRuntime/StrikersRecomp/runtimeharness — not dead,
deferred. Certificate tooling (demand_certificate.py, CERTIFICATE_G4QE01.md) remains valid and
feeds chassis specialization later.

**Mission.** DolRecomp output becomes a pluggable CPU core in an owned Dolphin fork. Any ISO gets
Dolphin's full 20-year hardware model; a per-game native module accelerates covered code; anything
uncovered (RELs, SMC, undiscovered indirect targets, mid-block oddities) falls back transparently.

**PRIME INVARIANT (the 100% game-agnostic gate, checked at EVERY arc end):** with no module
present, the chassis behaves exactly like stock Dolphin for any ISO. With a module present,
divergence = bug to fix; worst case is demotion to interpreter (slower, never broken).

**License/product note:** fork is GPLv2+; distribution = tooling + per-game modules, user provides
ISO (N64-ecosystem model). **Fork placement:** NEW top-level `dolphin-chassis/` (aurora-fork
precedent; `dolphin/` stays pristine read-only reference). Record upstream commit hash at fork time.

## Verified seams (read from source 2026-07-06 — do not re-derive)
- **Registration:** `JitInterface::InitJitCore` switch (JitInterface.cpp:47) constructs JIT-family
  cores; `PowerPC.cpp:208` selects. Work = new `CPUCore::StaticRecomp` enum value + case + config plumb.
- **Template:** CachedInterpreter (`class CachedInterpreter : public JitBase, ...` CachedInterpreter.h:25).
  Its `Run()` (CachedInterpreter.cpp:90) is the exact loop to mirror:
  `while Running { core_timing.Advance(); do ExecuteOneBlock() while (downcount>0 && Running) }`.
  `HandleFault → false` precedent = fastmem opt-out is legal.
- **State:** `PowerPCState` (PowerPC.h:120): pc/npc; `gpr[32]` u32 == our CPUState.gpr; `PairedSingle
  ps[32]` (PS0 full 64-bit double, PS1 32-bit semantics) vs our `f64 fpr[32] + ps1[32]`;
  `ConditionRegister cr` (u64 fields[8]) vs our packed u32 (shim at sync points only); UReg_MSR/FPSCR;
  `Exceptions` mask; downcount; **`gather_pipe_ptr` = built-in wgpipe fast path**; GQRs in spr[].
- **Memory:** MMU instance `Read_U8/16/32/64`, `Write_*` (+`_Swap`) (MMU.h ~:237); physical RAM
  pointer via Memory for the flat fast path.
- **Generated code facts** (StrikersRecomp/generated): per-guest-function C with per-instruction
  PC-dispatch switch → **mid-function re-entry (rfi/OSLoadContext) is native day one**; every
  cross-function transfer = `ctx->lr=K; ctx->pc=T; return;` (dispatcher round-trip); **NO cycle
  accounting emitted today** (verified — downcount is an arc-3 emission item); environment fully
  abstracted behind 7 CPUState fn-ptr hooks (GXRuntime include/core/cpu.h:49-55); SMC sites
  enumerated (generated_smc.txt); ABI must be versioned (G011 note).

## Design decisions (chosen up front; revisit only on evidence)
- **D1 state residency:** arc 1 = registers resident in our CPUState while native; sync pc/Exceptions
  each dispatcher iteration; FULL sync before any path that lets Dolphin touch state (exceptions,
  savestate, fallback entry/exit). CR packed↔fields[8] and fpr/ps1↔ps[i].ps0/ps1 conversion at sync
  points only. Migration path if profiling demands: regenerate against PowerPCState via adapter
  macros (fix_generated/DolRecomp `--chassis` profile) for zero-copy.
- **D2 fallback engine = Interpreter,** never JIT (always present on every platform incl. future
  JIT-banned targets; no block-cache interplay). Step until PC re-enters the native table or slice ends.
- **D3 downcount:** arc 1 = fixed approximate cost per dispatch (correctness safe — CoreTiming still
  owns time); arc 3 = real per-block `downcount -= n` emitted by DolRecomp/fix_generated.
- **D4 SMC/icache:** generated_smc.txt ranges permanently interpreter-routed; PLUS runtime guard —
  any icache invalidation touching a native-covered range demotes that function for the session
  (log + counter). Unknown SMC can never break a game, only slow it.
- **D5 module ABI:** per-game dylib (`g<GAMEID>_recomp`) exporting {abi_version, game_id, sorted
  addr→fn table, smc_ranges}; chassis autoloads by disc ID; absent → stock Dolphin. CPUState frozen+versioned.

## Arcs (each ends with the PRIME INVARIANT re-check + commit + ledger line)
**ARC 1 — skeleton boots.** Fork `dolphin-chassis/`; `CPUCore::StaticRecomp` + InitJitCore case;
`StaticRecompCore : JitBase` mirroring CachedInterpreter::Run; module loader (dlopen, ABI check);
dispatcher v1 (table hit → native w/ D1 sync; miss → interpreter steps); hooks v1 (RAM via physical
base + swap; everything else → MMU calls; mfspr TB/DEC → fallback). Build: generated chunks compile
into the Strikers module dylib. **Gate:** Strikers boots to menus with native-function counter > 0;
a second ISO (no module) boots identical to stock. Risks: hidden ppc_state couplings (feature_flags,
gather pipe) — keep fallback granular.
**ARC 2 — memory+hooks complete, coverage up.** All 7 hooks bound (PPCExternalPointer →
Memory::GetPointer); paired-single quantized load/store (GQR dequant) diffed vs Dolphin's tables;
gather-pipe fast path via ppc_state.gather_pipe_ptr; D4 SMC enforcement + demotion guard; cache-op
(dcbz/dcbf) semantics via MMU helpers. **Gate:** Strikers gameplay >90% native dispatches (measured
counter), zero unexpected demotions; invariant re-check. Risks: BAT/translation edge paths — route
all non-RAM through MMU, never guess.
**ARC 3 — timing/interrupts/exceptions.** Per-block downcount emission; external-interrupt check on
dispatcher back-edge; exception delivery = full sync → Dolphin vectors (vector code itself native);
rfi/OSLoadContext mid-function re-entry exercised; SingleStep() via interpreter; savestate full-sync
hooks. **Gate:** full match at correct speed, VI-paced; savestate round-trip mid-match; G008 PPC
differential suite green on chassis build. Risk: interrupt latency inside long native fns → optional
per-N-branches check emission.
**ARC 4 — lockstep differential + game matrix.** Debug mode: per dispatch, snapshot → run native →
rerun snapshot on interpreter → compare gpr/fpr/cr/pc/mem-writes; divergence dumps guest PC. Burn
down ALL divergences over Strikers boot→match. Run ≥2 additional ISOs (module-less) to re-prove
invariant. Measure fps vs stock JIT + coverage %. **Gate:** 0 divergences full match; numbers
documented. (This arc is where FPSCR/paired-single edge semantics die.)
**ARC 5 — product hardening.** Game-ID module packaging (one command); config toggle; dispatch perf
pass (sorted-table binary search / direct index — retires the G011 item-5 linear scan in chassis
context); interpreter-block memo for hot fallback ranges if measured; fork upstream-sync policy;
KNOWLEDGE writeup. **Gate:** distributable chassis + Strikers module; invariant statement verified.
**ARC 6 — reserved buffer** (typical consumers: FPSCR NI modes, XER carry edges, dcbz_l,
feature_flags interactions). Un-spent = win.

## Explicitly out of scope (4-6 arcs)
JIT-chained fallback · Wii titles · iOS build (design stays compatible via D2) · upstreaming ·
certificate-driven renderer thinning (later, on this same chassis) · MusyX HLE (Dolphin DSP covers audio here).

## Budget
Median arc 405k output tokens (measured) → program ≈ 2-2.5M output. Arc 1 is the riskiest;
if its gate slips past ~1.5 arcs of spend, STOP and re-evaluate against 03_GOLDEN_PATH (branch intact).
