# Arc 4 — lockstep differential burned to ZERO divergences (boot→attract-match)

**Date:** 2026-07-08 · **Goal:** G016 · **Arc:** 4 · **State:** core objective MET
(0 architectural divergences over Strikers boot→match); **gate NOT fully passed** — 4.7
(Melee + oracle.dol module-less invariant) and 4.8 (fps/coverage vs `-C Dolphin.Core.CPUCore=1`)
were NOT run (user chose to wind down here).

## Result
`STATICRECOMP_LOCKSTEP=1`, Strikers boot → attract-demo match, **5+ minutes** (v12:
`native=24.6B`, `wall=323s`, `smc_failed=1`): **0 DIVERGE, 0 CTRLFLOW, 0 `vmem[`, 0 `lc[`,
0 `mmio#`, 0 read-overflow.** Only benign D3 `UNDERCHARGE` notes (64 reported = report cap,
worst `deficit=71` cycles, every one `regs/mem exact`). The recomp is value-bit-exact against
Dolphin's interpreter over the whole boot→match path.

The 14-report residual from session-2's handoff was **entirely harness-fidelity** (the recomp
had ZERO real bugs). This session found the root causes and fixed the HARNESS to align with
native's actual semantics, rather than whitelisting.

## The 5 fixes (all in `dolphin-chassis/`, none touch the recomp/emitter)

1. **Write-compare scope — classify by post-translation physical.**
   Native's `HookExternalWrite`/`Read` recorded ALL external accesses as "MMIO", but the
   interpreter's shadow sink only captures gather-pipe + MMIO. VM-window and locked-cache
   accesses are MEMORY. `LsHwAccessInScope(mmu, ea)` now translates via
   `mmu.GetTranslatedAddress(ea)` and classifies the PHYSICAL (gather 0x0C008000 mask, MMIO
   `(phys&0xF8000000)==0x08000000`); everything else is memory, journaled/compared separately.
   Cleared 101 false `mmio#:N=X,I=0`. Read side (`m_ls_native_reads`) uses the same gate so
   VM/LC reads (served live from the restored pre-image) don't desync the HW read-replay index.

2. **Locked-cache (L1Cache 0xE0000000) pre-image journal** — `g_lc_write_journal` in MMU.cpp's
   LC branch + native/shadow trampolines + restore/compare/redo in `LockstepCheck` (mirrors MEM1).

3. **Fake-VMEM (guest VM window) pre-image journal — THE stale-read root cause.**
   The guest virtual-memory window `[0x7E000000, 0x80000000)` (nlMemory `VMAlloc`, BAT-driven)
   maps to Dolphin's **separate FakeVMEM buffer, NOT MEM1** (`MMU.cpp` WriteToHardware line ~548,
   `GetFakeVMEM()`). Native's read-modify-write there (e.g. `glSetRasterState`: `lwz r8,0(r3);
   andc; stw r7,0(r3)` clearing a flag bit; `nlListAddStart`: list-pointer store) was
   **un-journaled** — bypassed both the `ctx->ram` module hook AND the MEM1 MMU journal — so the
   pre-image restore never undid it and the shadow read native's **POST-write** value
   (glSetRaster r8 N=0xc2014 vs I=0xc2004; nlListAddStart r0 N=0 vs I=0x815e1f50). Added
   `g_vmem_write_journal` (`VmemWriteJournal`), `m_ls_vmem_pre/_post/_shadow_pre`, native/shadow
   trampolines, and restore/compare/redo mirroring L1Cache. Offset = `em_address & GetFakeVMemMask()`.
   **This buffer is the trap: any future "shadow reads stale value" divergence on a 0x7Exxxxxx
   address is a missing FakeVMEM journal, not a recomp bug.**

4. **Read-replay classification** (see #1) — native records only true-hardware reads.

5. **Loop-header `end_pc` alignment** — the dominant loop-align class (strcmp + all ctr/pointer
   off-by-one). Native emits a backward branch (loop back-edge) as a dispatcher round-trip
   (`ctx->pc=T; return;`) but keeps the loop's forward ENTRY as a local `goto`. So for an
   `end_pc` that is a loop header, native returns via the **back-edge** (one iteration in), while
   the shadow single-steps and reaches the same `end_pc` FIRST via the forward/fall-through
   ENTRY — stopping one iteration early. Aggravated by D3 undercharge (native's charge omits the
   mid-accounting-block prologue, so the shadow's cycle budget is exhausted right at the loop
   header). Fix:
   - `LsIsLoopHeader(ram, ram_size, end_pc)` scans forward (2048-insn window, NO early break —
     an interior unconditional `b` must not cut off a far back-edge, e.g. CalcDesiredTarget's is
     116 insns down) for a direct `b`/`bc` (AA=0) back to `end_pc`.
   - Non-seq stop (line ~1095) and budget stop (line ~1113): for a loop-header `end_pc`, skip the
     loop-ENTRY arrival and wait for the back-edge (`before > end_pc`). The entry-skip fires only
     for a true LOCAL forward goto (same-chunk, non-linking, non-indirect); a `bl`/indirect/
     cross-chunk arrival is a genuine dispatcher round-trip boundary and is KEPT (regression fix:
     `GetLinearVelocity`/`GetAngularVelocity` are `bl`-called functions that loop back to their
     own entry — an early over-aggressive skip caused a CTRLFLOW flood at their entries).
   Cleared strcmp + #3/#4/#5/#6/#7 with **zero** CTRLFLOW regressions.

## How each was found (methodology that worked)
- Read the **generated C** for the divergent block — it is the exact ground truth of native's
  control flow (goto vs `ctx->pc=…;return;`). This killed the loop-align mystery deductively
  (block #4's `bdnz` back-edge + missing prologue charge = ctr N=2/I=3, exactly).
- For the stale-read, a targeted `[ls-mem]` dump (pre-image + read-addr translation) + the
  per-instruction `STATICRECOMP_LOCKSTEP_TRACE` showed **m_ls_pre EMPTY** for the glSetRaster
  block despite a `stw` — proving the write bypassed journaling → traced it to the FakeVMEM
  branch in MMU.cpp. (The `[ls-mem]` diagnostic has been REMOVED; code is clean.)

## Exact next step (RESUME ARC 4 here — gate not passed)
1. **4.7 module-less invariant** (PRIME INVARIANT re-proof): run the SAME nogui binary with NO
   module + default core on ≥2 more ISOs and confirm byte-identical to stock:
   - Melee: `Super Smash Bros. Melee (USA)...iso` at repo root.
   - oracle DOL: `DolRecomp/tests/oracle/oracle.dol`.
   Drop `STATICRECOMP_MODULE` and `-C Dolphin.Core.CPUCore=6` (default core); or compare
   CPUCore=6-with-no-module vs CPUCore=1.
2. **4.8 fps/coverage vs stock JIT:** same Strikers run with `-C Dolphin.Core.CPUCore=1` (stock
   JITARM64, no module) vs CPUCore=6+module; measure fps (FPS-overlay flags) and in-match native
   dispatch % (verbose `native/fallback` counters, expect ~99.9%). Document the numbers.
3. Then arc-4 GATE = "0 divergences full match; numbers documented" is fully met → arc-4 done.

## Baselines to re-verify before declaring arc-4 done (NOT re-run this session)
oracle 239/0, dedicated 26/0, GXRuntime 16/16, DolRecomp ctest 10/10; standalone Strikers boots.

## Build + run (unchanged)
Build: `ninja -C dolphin-chassis/build dolphin-emu-nogui`.
Lockstep run: `env STATICRECOMP_MODULE=…/StrikersRecomp/build-chassis-module/gG4QE01_recomp.dylib
STATICRECOMP_LOCKSTEP=1 STATICRECOMP_VERBOSE=1 STATICRECOMP_LOCKSTEP_MAXREPORT=500 …/dolphin-emu-nogui
-e "…/Super Mario Strikers (USA).iso" -u …/.tools/dolphin/user -v Metal -C Dolphin.Core.CPUCore=6`.
Trace one block: add `STATICRECOMP_LOCKSTEP_TRACE=0x<entrypc>`.
Changes committed on branch **`arc4-lockstep-zero`** in `dolphin-chassis` (4 files: MMU.cpp,
StaticRecompCore.{h,cpp}, StaticRecompLockstep.h).
