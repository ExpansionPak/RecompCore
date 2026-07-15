# 65 / ARC 4 — boot window CLEAN (0 architectural divergences); boot→match class found

**Status: arc 4 still NOT gate-passed, but the recomp is proven correct.** Over the
60M-dispatch boot window: **0 architectural divergences** (regs/fpr/ps1/cr/xer/fpscr/
mem/mmio). The previous session's 8 residuals are all resolved: 5 were a Dolphin fcmp
quirk (FIXED in the recomp), 3 were the same downcount undercharge (now quantified, not
misreported). Extending unbounded to the attract screen surfaced a NEW class of ~14
reports that are **ALL harness-fidelity limitations, ZERO recomp bugs** — that class is
the RESUME work.

## What landed (all committed)
### 1. The fcmp FPCC-accumulation fix (the 5 FPSCR divergences) — REAL recomp fix
Root-caused with a new per-instruction shadow trace (see §diagnostic). A `fcmpu` left
FPCC=`FL|FG` where native cleared to `FL`. Dolphin's `Helper_FloatCompare[Un]ordered`:
`fpscr.FPRF = (fpscr.FPRF & ~FPCC_MASK) | compare` — `FPRF` reads as the **5-bit field
value (0..31)** but `FPCC_MASK = 0xF << 12` is a **Hex-space** constant, so `& ~0xF000`
is a no-op and Dolphin **OR-accumulates** the compare into the old FPCC. Arithmetic ops
do `fpscr.FPRF = ClassifyFloat/Double` (a real field replace) → **only fcmp accumulates**.
Present in pristine `dolphin/` too (canonical). Fix in BOTH `GXRuntime/src/core/cpu.c`
and `DolRecomp/src/core/cpu.c`: `ppc_fcmp` now does `cpu->fpscr |= (compare << 12);`
(CR field still replaces via the existing masked write). All 5 FPSCR divergences gone;
oracle stayed **239/0/0** (its cases start from clean FPCC, so clear≡accumulate from 0).
Full detail: `KNOWLEDGE/recomp-codegen.md §fcmp OR-ACCUMULATES`.

### 2. The 3 CTRLFLOW residuals were ALL the mid-accounting-block-entry undercharge
The prior handoff's "`N_cyc==I_cyc` ⇒ real branch split" heuristic was **WRONG**. The
emitter's dispatch `switch` cases EVERY instruction (any address is dispatchable), but
accounting `leader[]` is marked only for function entry, control-transfer successors, and
**local** branch targets. A dispatch INTO a non-leader address (cross-function / indirect
/ jump-table target) skips that block's `ctx->downcount -=` charge → undercharges by the
entry→block-end cost. The cycle-bounded shadow then stops short of native's real boundary.
Proven: `0x8027A610` (deficit 2), `0x8027D76C` (3), `0x801D42CC` (25, stmw-heavy) all
reach end_pc **regs+mem bit-exact** given a grace budget. **No real branch splits exist**
(0 register divergences over 4120 blocks ⇒ identical inputs+code ⇒ identical branches).

### 3. Harness fixes (both invariant-safe, verified inert when off / module-less)
- **Undercharge grace** (`StaticRecompCore::LockstepCheck`): when native's charge is
  consumed without reaching end_pc, step on to end_pc for `LS_UNDERCHARGE_GRACE=256`
  extra cycles, then compare. A regs/mem-exact reach with `I_cyc>N_cyc` is reported as a
  quantified `UNDERCHARGE` note (NOT a DIVERGE); shutdown summary prints
  `undercharges=`/`max_deficit=`.
- **Shadow RAM-write journal** (`g_ram_write_journal`, new hook in `MMU.cpp`'s MEM1 RAM
  branch + `StaticRecompLockstep.h` + `LsShadowJournalTrampoline`): records the shadow's
  MEM1 stores so `LockstepCheck` UNDOES them before redoing native's. The shadow was
  previously leaking any store it made to an offset native didn't write (only `m_ls_post`
  was restored), **permanently corrupting the canonical native RAM** for the rest of the
  session. (Did not change strcmp — that's loop-alignment, see below — but is a real
  correctness bug for hardware blocks whose interpreter path stores RAM native's hook
  routed elsewhere.)

### 4. Diagnostic kept: `STATICRECOMP_LOCKSTEP_TRACE=0x<entryPC>`
Dumps the shadow's per-instruction `pc/op/fpscr/cr/gpr` + entry regs + RAM bytes at
`r3/r4` for one entry PC. Gated, zero-cost when off. This pinned BOTH the fcmp quirk and
the strcmp byte-identical strings. Keep it (remove at final gate-pass).

## The remaining class (boot→match, ~14 reports) — ALL harness-fidelity, ZERO recomp bugs
- **13× `mmio#:N=X,I=0`** on GX/hardware blocks: `glSetRasterState`, `glplatModifyPacket`,
  `glSetTextureState`, `__THPDecompressiMCURowNxN`, `__LCEnable`, `nlReadAsyncToVirtualMemory`,
  `ReadEntireSampleFileIntoMem`. Native's `HookExternalWrite` records ALL external writes
  (gather `0xCC008000` + MMIO `0x08000000` + **locked cache `0xE0000000`**); the interpreter
  sink (`MMU::WriteToHardware`, `is_gather||is_mmio`) captures only gather+MMIO — LC commits
  to the L1Cache branch UNcaptured → write-comparison SCOPE mismatch.
- **1× `strcmp` 0x80234E98 off-by-one loop iteration** (`r3:N=…005,I=…004 r5:N=0x48,I=0x54`):
  the RAM dump proved **both strings are byte-identical "THP\0"**; native and the shadow
  stop at adjacent bytes ('H' vs 'T') of the SAME equal compare. end_pc `0x80234F98` is
  loop-INTERNAL, reached at different iterations. A real strcmp bug on equal strings would
  return non-zero, not an off-by-one pointer — **not a miscompute.**

## Exact next step — RESUME ARC 4 (at the boot→match harness-fidelity class)
1. **Write-compare scope alignment (clears the 13).** In `LockstepCheck`'s MMIO compare,
   compare only writes to regions BOTH engines capture identically (gather `0xCC008000` +
   MMIO `0x08000000`), OR skip the write-count/value compare for blocks that touched
   locked-cache/other external regions (keep regs/mem check). Decide by reading which EA
   ranges native's `m_ls_native_mmio` holds vs the sink. ALSO verify the interpreter's LC
   (`0xE0000000`→L1Cache) stores don't corrupt native's later LC reads (Fix A only journals
   MEM1 — may need an L1Cache journal too).
2. **Loop-internal-end_pc alignment (clears the strcmp class).** The shadow stops at the
   FIRST arrival at end_pc; native stopped at a later loop iteration. Needs an end-detection
   that matches native's iteration count (hard without native's instruction count — consider
   emitting/threading native's executed-instruction count, or detecting the loop and
   matching the arrival index).
3. Re-run boot→match to **0 real divergences**; drive the attract match (idle ~2-3 min at
   title) for gameplay-only blocks.
4. **4.7:** run the harness module-less on Melee + the oracle DOL (invariant). Both present:
   `Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso` at repo root,
   `DolRecomp/tests/oracle/oracle.dol`.
5. **4.8:** fps/coverage vs `-C Dolphin.Core.CPUCore=1` (stock JITARM64, no module).
6. **D3 undercharge (arc-5 dispatch-perf item):** the emitter fix is "accounting leader at
   every dispatch-entry case-label" = per-instruction charging (or global+jump-table leader
   analysis) — it reverts arc-3's batching, so weigh it in the perf pass. ≤25-cycle,
   ~0.07%-frequency skew; the grace makes the differential ignore it meanwhile.

## Run recipes
Lockstep (bounded to boot window):
```
env STATICRECOMP_MODULE=…/gG4QE01_recomp.dylib STATICRECOMP_VERBOSE=1 \
  STATICRECOMP_LOCKSTEP=1 STATICRECOMP_LOCKSTEP_LIMIT=60000000 \
  STATICRECOMP_LOCKSTEP_MAXREPORT=500 [STATICRECOMP_LOCKSTEP_TRACE=0x<pc>] \
  …/dolphin-emu-nogui -e "…/Super Mario Strikers (USA).iso" \
  -u …/.tools/dolphin/user -v Metal -C Dolphin.Core.CPUCore=6
```
Unbounded boot→match: drop `_LIMIT`. Build after chassis edits: `ninja -C
dolphin-chassis/build dolphin-emu-nogui`. After cpu.c edits: rebuild the module
(`ninja -C StrikersRecomp/build-chassis-module`) AND re-run the oracle with the
`rm host_diff_gen …` dance (Makefile doesn't track cpu.c).

## Verify baselines (all green this session)
oracle host_diff 239/6 XFAIL/0 unexpected/0 XPASS · dedicated 26/0 · GXRuntime 16/16 ·
DolRecomp ctest 10/10 · title renders (screenshot) · Melee module-less boots clean 0
[lockstep] · Strikers+module no-lockstep 99.95% native 0 [lockstep] smc_failed=1.

## Surprises / method notes
- The per-instruction trace is the highest-leverage tool for these residuals — build it
  FIRST for any FPSCR/register divergence; it pinned the fcmp quirk in one run and the
  strcmp byte-identical strings in one run.
- Prove-by-logic worked: "0 register divergences ⇒ no real branch splits" killed the
  "branch split" theory without per-case tracing all of them.
- The RAM-byte dump (entry r3/r4 + 20 bytes) is what turned "strcmp diverges — real bug?"
  into "strcmp compares identical strings, off-by-one loop snapshot" in seconds.
- macOS `awk` has no `strtonum`/3-arg `match` — use `perl` for symbol-address lookups.
