# Arc 4 — GATE PASSED (4.7 module-less invariant + 4.8 fps/coverage)

**Date:** 2026-07-08 · **Goal:** G016 · **Arc:** 4 · **State:** GATE FULLY MET → **ARC 4 COMPLETE.**
Supersedes `2026-07-08_arc4_lockstep-zero-divergence.md` for current state (that arc closed the
lockstep to 0; this session ran the two remaining gate items 4.7 + 4.8).

## What this session did (pure measurement — ZERO code changes)
The lockstep differential was already at 0 architectural divergences (prior session, on branch
`arc4-lockstep-zero`). Remaining gate items were 4.7 and 4.8. Both done.

### 4.7 — module-less PRIME INVARIANT re-proof (module absent ⇒ behaves like stock)
The `.tools/dolphin/user` dir has **no `StaticRecompModules/`**, so a run with no `STATICRECOMP_MODULE`
env loads nothing = the true invariant path. Ran two arbitrary targets beyond Strikers on CPUCore=6:
- **Melee (GALE01)** — arbitrary non-Strikers retail ISO. CPUCore=6 no-module → **native=0 /
  fallback=8.27B** (every dispatch a Dolphin `Interpreter::SingleStepInner` step), boots to a correct
  **title screen** (screenshot `/tmp/melee_c6.png`), no exception. Stock control CPUCore=1 (=JITARM64)
  renders the intro cinematic correctly (`/tmp/melee_c1.png`).
- **oracle.dol** (`DolRecomp/tests/oracle/oracle.dol`, a devkitPPC GC exe) — CPUCore=6 no-module →
  native=0, boots clean, converges to a stable idle loop at **0x80007154** (the opcode producer
  reports over USB-Gecko/TCP, which isn't wired here → it idles after running its cases), no
  exception. Stock CPUCore=1 runs it clean too (alive, executing, no error).

### 4.8 — fps + coverage vs stock JIT (Strikers attract-demo match, FPS overlay on)

| metric | chassis CPUCore=6 + module | stock JIT CPUCore=1, no module |
|---|---|---|
| native dispatch coverage | **99.908%** (24.30B / 22.3M fallback) | n/a |
| Speed | 97% | 100% |
| Max (headroom) | 97% (CPU-bound, no headroom) | **379%** (~3.8× headroom) |
| VPS | 57.89 | 59.95 |
| FPS (game present) | 29.20 (heavy goal-cam, ½-VI cadence) | 59.95 (kickoff, per-VI) |
| render | correct (Toad+ball+Kritter goalie) | correct |

Screenshots `/tmp/strikers_c6.png`, `/tmp/strikers_c1.png`. smc_failed=1 (expected VMBASE chunk).
**Read the chassis by Speed/VPS/native%, not FPS** — FPS is the game's own present cadence
(scene-dependent). The chassis runs near-real-time with 99.9% native coverage and correct output;
the ~3.8× JIT headroom is the honest dispatch-perf gap → **arc-5** (per-dispatch burst-loop overhead +
D3 downcount batching). Arc-3 baseline for lighter gameplay stays ~100%/Max~113%/60fps.

## Baselines re-verified GREEN this session
DolRecomp ctest **10/10**; GXRuntime ctest **16/16**; oracle host_diff **239 cases, 0 unexpected**
(6 known stswi/stswx `normalize_pointer` XFAIL, USER-flagged); oracle dedicated **26/0**;
StrikersRecomp git-clean (standalone-boots baseline unchanged; nothing this arc touched it).

## Gate check
ARC-4 gate (00_PROGRAM.md): "**0 divergences full match; numbers documented**" — both met.
PRIME INVARIANT re-checked on 2 arbitrary targets. **Arc 4 is done.**

## Repo state
No code changed this session. `dolphin-chassis` stays committed on branch **`arc4-lockstep-zero`**
(last commit `2fc955dba8`). runtimeharness updated: goals.json G016 evidence, KNOWLEDGE/
dolphin-chassis.md (§Arc-4 GATE PASSED), this handoff, ledger. **Branch not merged** — left for the
user / arc-5 start to decide the chassis branch-merge convention.

## NEXT: ARC 5 — product hardening (00_PROGRAM.md)
Game-ID module packaging (one command); config toggle; **dispatch perf pass** (sorted-table binary
search / direct index — retires the G011 linear `dolrecomp_call` scan in chassis context; this is
where the 3.8× headroom gap gets closed); interpreter-block memo for hot fallback ranges if measured;
fork upstream-sync policy; KNOWLEDGE writeup. Gate: distributable chassis + Strikers module;
invariant statement verified. Arc-6 = reserved buffer (FPSCR NI modes, XER carry edges, dcbz_l,
feature_flags). Run/observe recipes: KNOWLEDGE/dolphin-chassis.md §Arc-4 GATE PASSED + §Observation.
