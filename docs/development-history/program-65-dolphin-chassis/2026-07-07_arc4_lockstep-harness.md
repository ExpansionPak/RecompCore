# 65 / ARC 4 — lockstep harness built + divergences burned 100→8 (RESUME arc 4 at the 8 residuals)

**Status: arc 4 still NOT gate-passed.** 4.5 (the lockstep harness) is DONE and working.
4.6 (burn-down) is ~90% done: from a first run of 100 divergences down to **8
characterised residuals**, all now needing per-case operand-level tracing OR
belonging to arc-3 (downcount). 4.7 (Melee + oracle-DOL module-less matrix) and
4.8 (fps/coverage vs CPUCore=1) are NOT started. Wound down on the context
blocker that each of the 8 residuals is its own mini-investigation.

## THE HEADLINE (evidence, unambiguous)
Over the first 60M-dispatch window (4120 distinct blocks differentially checked):
**ZERO fpr / ps1 divergences.** The Dolphin-exact FP/paired-single unit is
value-bit-exact IN-GAME, not just in the oracle. Every residual is a *status
bit* (FPSCR FPCC), a *branch*, or a *cycle-charge* discrepancy — never a computed
FP value. Grounding also re-confirmed: boot→attract-match native **99.92%**,
smc_failed=1, correct match render, ~59 FPS (screenshot verified).

## What landed (4.5 — the harness). Files, all committed:
- **GXRuntime `src/core/cpu.c`**: a null-guarded RAM-write journal —
  `ppc_set_mem_write_journal(fn,user)` + a call before every `write_beN`/byte
  store. Zero-cost when unset; the chassis resolves the setter by dlsym (absent
  ⇒ lockstep auto-disables). NOT part of the CPU ABI. This is what lets the
  harness restore a *pre-block* memory image so read-modify-write blocks compare.
- **dolphin-chassis `StaticRecompLockstep.h`** (new): the shared hook surface —
  `g_hw_write_sink` (suppress+capture interpreter MMIO/gather writes),
  `g_hw_read_sink` (replay native's recorded MMIO reads), `g_tb_override_*` (pin
  timebase). All null/false outside a shadow ⇒ invariant untouched, module-less
  runs never set them.
- **MMU.cpp**: in `WriteToHardware`, if the write sink is set and the (post-
  translation) address is gather/MMIO, record + return WITHOUT committing (no
  double GX FIFO / PI side effects). In `ReadFromHardware`, if the read sink is
  set, return native's replayed value for the MMIO branch (no drift / double
  read). Both guarded by a single null pointer test.
- **Interpreter_SystemRegisters.cpp**: `mftb`/`mfspr TL/TU` returns the pinned
  `g_tb_override_value` during a shadow instead of live `GetFakeTimeBase()`.
- **StaticRecompCore.{h,cpp}**: `STATICRECOMP_LOCKSTEP=1` enables. Per FIRST
  dispatch of each distinct entry PC (deduped in `m_ls_checked`), within the
  `[_START,_LIMIT)` native-dispatch window: snapshot CPUState S; journal native's
  RAM writes + capture native MMIO reads/writes at the hooks + note if native hit
  the instruction fallback; run native (canonical); then re-run the block on
  `Interpreter::SingleStepInner` from S with writes suppressed / reads+TB
  replayed; compare gpr/fpr(ps0+ps1)/cr/xer/fpscr/msr/lr/ctr/srr/pc + RAM writes
  (native journal post-images vs realRAM) + MMIO writes (native hooks vs sink);
  report entry PC + diff fields; restore. Env: `_START`,`_LIMIT`,`_MAXREPORT`,
  `_STEPCAP`,`_WHITELIST`. Shutdown prints `checks/reports/skipped_fallback/
  skipped_zero/distinct_pcs`.

### The stopping condition (the crux — read `recomp-codegen.md`/`dolphin-chassis.md`)
Native's `dispatch` runs ONE segment ending at a control transfer, so `end_pc`
is a JUMP TARGET that a loop body can pass through *sequentially* first. Stop the
shadow when: (a) it reaches end_pc via a NON-sequential transfer (branch/loop
back-edge/exception vector = native's boundary), OR (b) it has consumed native's
exact charged cycles (`-m_guest.downcount` at check entry; a straight-line
segment end). The cycle bound also CAPS the shadow so it can never overshoot into
data / illegal opcodes. Zero-charge dispatches are skipped (`skipped_zero`).

## Burn-down chronology (each step = one build+run, verified)
1. **CRASH #1: invalid free.** `std::memcpy` of the whole `PowerPCState`
   double-frees its non-trivial `iCache`/`dCache` members at scope exit. FIX:
   save/restore ONLY `msr`+`downcount`+`Exceptions` (all other ppc regs are
   stale mid-burst and rebuilt at SyncOut). → ran clean, **100** divergences.
2. **Timebase drift** (OSGetTick `0x80259C44` r off-by-1, `0x595…`). FIX: pin
   shadow TB to `entry_state.timebase`. Killed ~3 (100→~98).
3. **Loop miscount** (dominant): stopping at first `pc==end_pc` halted the
   interpreter a whole loop iteration early (e.g. `0x802488C4`: setup falls
   through to loop-top end_pc `0x802488D8`, native ran setup+1 iteration). FIX:
   cycle-bounded stepping. 98→**21**.
4. **CRASH #2: `unknown_instruction`.** The interim "non-sequential only" rule
   overshot at straight-line segment ends. FIX: combined pc+cycle stop above.
5. **MMIO read drift + write-mask artifact**: shadow re-read live hardware
   counters; and I compared the interpreter's raw 32-bit store register against
   native's width-masked hook value. FIX: MMIO **read replay** (record native's
   `HookExternalRead[32]` values, replay in order via the read sink) + mask
   captured writes to `size`. 21→**8**.

## The 8 residuals (ls7.log) — the RESUME work
All 4120 blocks; `skipped_fallback=27 skipped_zero=17`.
- **5× FPSCR (FPCC bit 0x4000 / FI 0x20000), CR matches, fpr matches**, all on
  huge-span call blocks (entry>end, e.g. `0x80251798`→`0x8024BC58`). `ppc_fcmp`
  and Dolphin `fcmpu` clear FPCC identically, so it is NOT the single visible
  fcmpu — the block runs many instrs to a far branch and a LATER FP op / stale
  FPCC is the culprit. NEEDS: a one-shot per-instruction FPSCR dump for one such
  entry PC (add temp trace keyed on entry==0x80251798) to find the exact op.
  Candidate causes to check: an arithmetic FP op's FPRF update, or a host-FP-mode
  arming delta (native `ppc_arm_host_fp_mode` vs Dolphin `RoundingModeUpdated`).
- **#8 CTRLFLOW `N_cyc=3,I_cyc=13` @ `0x801D42CC`** (`Set__6ConfigFPCcPCc`+0xC):
  the block (`stfd;psq_st;stmw;…`) has NO `ctx->downcount -=` at its label — its
  accounting-block charge sits at an EARLIER label. Native's burst dispatched
  INTO 0x801D42CC (a round-trip target mid-accounting-block) and skipped that
  charge ⇒ real **downcount UNDERCHARGE on mid-block entry** (arc-3/codegen). The
  cycle bound then stops the shadow early ⇒ shows as CTRLFLOW. Registers almost
  certainly match. This is a genuine timing-accuracy finding the harness surfaced.
- **#6/#7 CTRLFLOW `N_cyc==I_cyc`, pc differs** (`0x8027A610`→`0x80278494`,
  `0x8027D76C`→`0x8023A718`): a branch went a different way at equal cycles ⇒ a
  real control-flow split with identical replayed inputs. NEEDS per-instruction
  trace to find which register/flag diverged at the branch.

## Known harness limitation (document, do not "fix" blindly)
Cycle-bounding CONFLATES downcount-charge bugs (#8) with control-flow
divergences: when native's charge is wrong, the shadow stops before end_pc and
reports CTRLFLOW even if registers would match. Distinguishing them needs native's
true instruction count (not available without a codegen/ABI change) — accept the
conflation and read `N_cyc` vs `I_cyc` in the report (equal ⇒ real branch split;
unequal ⇒ suspect charge). Do NOT drop the cycle cap: it is what prevents the
`unknown_instruction` overshoot crash.

## Exact next step — RESUME ARC 4
1. Add a temporary per-instruction dump (pc, op, fpscr, cr) gated on a single
   entry PC (`STATICRECOMP_LOCKSTEP_TRACE=0x80251798`); run windowed; identify
   the FP op that sets the extra FG bit. Fix in `ppc_fcmp`/the arithmetic FPRF
   path OR prove it a host-FP-arming delta and align the arming. Re-run: expect
   the 5 FPSCR to vanish.
2. Same trace on `0x8027A610`/`0x8027D76C` for the branch splits.
3. File #8 (mid-block-entry undercharge) as an arc-3 downcount item; decide if
   the harness should special-case it (skip when the entry PC label carries no
   charge) so it stops reporting.
4. Widen the window (remove `_LIMIT`) for a full boot→match pass; drive a match
   (attract demo) and re-run to catch gameplay-only blocks; burn to 0.
5. 4.7: run the harness module-less on Melee + the oracle DOL (invariant); 4.8:
   fps/coverage vs `-C Dolphin.Core.CPUCore=1`.

## Run recipe (lockstep)
```
env STATICRECOMP_MODULE=…/gG4QE01_recomp.dylib STATICRECOMP_VERBOSE=1 \
  STATICRECOMP_LOCKSTEP=1 STATICRECOMP_LOCKSTEP_LIMIT=60000000 \
  STATICRECOMP_LOCKSTEP_MAXREPORT=250 \
  …/dolphin-emu-nogui -e "…/Super Mario Strikers (USA).iso" \
  -u …/.tools/dolphin/user -v Metal -C Dolphin.Core.CPUCore=6 > log 2>&1 &
```
Lockstep is inert without `STATICRECOMP_LOCKSTEP` (invariant re-verified: a
no-lockstep run is byte-identical, 99.94% native, zero `[lockstep]` lines).

## Surprises / method notes
- Two crashes, both mine, both in `LockstepCheck`: the PowerPCState memcpy
  (non-trivial cache members) and the end-detection overshoot. Neither is a
  recomp bug. Get the FULL crash report (`~/Library/Logs/DiagnosticReports/
  dolphin-emu-nogui-*.ips`, Thread "CPU-GPU thread") — the truncated pasted one
  hid the offending frame.
- The user interrupts long `sleep` polls to hand back a crash report — that's the
  fastest signal; read the .ips immediately rather than re-running.
- Input replay (TB + MMIO reads) is THE technique that turns a noisy differential
  (100) into a clean one (8): feed both engines identical hardware inputs, and
  only genuine recomp/codegen deltas remain.
