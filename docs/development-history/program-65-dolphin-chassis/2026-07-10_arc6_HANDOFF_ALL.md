# ARC 6 HANDOFF — everything in one file (2026-07-10, wind-down mid-arc)

**Session goal (user, /goal):** RecompCore (dolphin-chassis StaticRecomp core) must run FASTER
than stock JIT on covered code; AT MINIMUM match JIT max speed (reference: stock JITARM64
Speed 100% / Max 379% vs chassis 95-100%, arc-4 §4.8). "You cannot leave anything undone."
This file is the ONLY wind-down artifact (user asked for one file instead of the usual
KNOWLEDGE/goals/handoff split). goals.json G016 NOT updated this session; KNOWLEDGE not
updated; one ledger pointer line appended. Fold the durable facts below into
KNOWLEDGE/dolphin-chassis.md + goals.json at arc end.

**STATE IN ONE SENTENCE:** the entire v3 fast-dispatch stack (emitter + ABI + module + chassis)
is implemented, builds, boots Strikers, and shows ~2x speed on the worst-case boot scene with
the perf ceiling clearly reachable — but the lockstep differential is RED (23 CTRLFLOW reports
in a 45s boot window) with a fully-instrumented, deterministic, 45-cycle repro waiting for one
probe run that the Bash-tool outage blocked; do NOT ship/measure further until lockstep is 0.

---

## 1. What was designed and why (decisions are FINAL unless lockstep forces otherwise)

Root causes of the old ~4x gap (all verified in source/asm):
1. Per-instruction `case` + `label_` + `ctx->pc=` stores in emitted chunks made every
   instruction a switch-reachable join point ⇒ compiler kept ALL guest state memory-coherent
   at every instruction ⇒ interpreter-grade codegen.
2. Every backward branch and every call/return was a dispatcher round-trip through the chassis
   burst loop (~19% of thread) + cross-dylib dispatch + 4096-case switch.
3. Out-of-line helper calls for every memory access (mem_read32 ~10%) and an out-of-line
   `ppc_fp_available` call before EVERY FP instruction forced full spills around each.

**v3 model ("hot chunk"):**
- **CPU ABI v3** (GXRUNTIME_CPU_ABI_VERSION 3): `ctx->downcount` = environment-PRELOADED
  positive cycle budget. Leaders charge `downcount -= cost`; BACKWARD local branches and ALL
  call sites check `<= 0` and return to the dispatcher. consumed = preload − remaining.
  Mid-block exception exits REFUND the unexecuted suffix (faulting instruction stays charged —
  Dolphin's SingleStepInner has a single exit returning opinfo cycles, verified in source).
  New tail field `u32 host_depth` + DOLRECOMP_MAX_HOST_DEPTH=192: `bl` sites stop nesting host
  frames at depth 192 and degrade to dispatcher round-trips (guest recursion can never blow the
  host stack).
- **Graceful degradation:** hosts that leave budget at 0 (standalone StrikersRecomp) get exact
  v2 per-block dispatch — every check fails immediately. RULE: hosts that install `host_call`
  (HLE interception) MUST NOT preload (chained native calls would bypass address interception).
  Chassis has host_call=NULL. Standalone is therefore UNCHANGED behaviorally (G011 can adopt
  budgets later).
- **Leaders-only case labels**: leaders = region entries + entry_point + transfer successors +
  direct branch targets, computed GLOBALLY across sections (chunks split functions at arbitrary
  16KB boundaries; cross-chunk/cross-section branch targets MUST be leaders — a per-chunk pass
  would silently break them; jump-table case bodies are covered by the transfer-successor rule).
  Case-label set == accounting-leader set ⇒ every possible dispatch entry charges its block ⇒
  the arc-4 "D3 mid-block-entry undercharge" class is structurally dead. Hot chunk 0x802596C0
  went 4096 → 883 cases. Non-leader re-entry (exception-retry cia) → chassis interpreter-steps
  to the next leader (DispatchableAt/enterable returns false); standalone treats as uncovered
  (same as today for flows it actually has).
- **Emitted control flow:** local backward b/bc = budget-check + `goto` (native loops!); local
  forward = goto (unchanged); `bl/bcl` = host-stack guarded call:
  `lr=R; pc=T; if(budget<=0||depth>=192)return; [if(!CHUNK_OK(ci))return;] depth++;
  func_TCHUNK(ctx); while(pc!=R){ if(budget<=0||exception||!DOLRECOMP_ENTER(ctx)){depth--;return;} }
  depth--;` then falls through to label_R (a leader, which charges). Any non-local flow
  (context switch, exception, rfi) fails the resume check and unwinds every frame to the
  dispatcher — the host stack is a pure accelerator, never a semantic dependency. The while-loop
  keeps call trees native across indirect flow inside callees. `bl target==resume` (get-pc idiom)
  = just `lr=R` fallthrough. bclrl/bcctrl = same loop with dynamic first entry via
  DOLRECOMP_ENTER. **blr = `pc=lr; return;`** (host-return matches the caller's while).
  **Non-local `b` and `bctr` = `pc=T; return;` — deliberately NOT direct calls: tail-transfer
  chains through frames would grow the host stack unboundedly (state-machine ping-pong);
  the enclosing call-loop / dolrecomp_call chain loop re-enters natively, iteratively.**
- **dolrecomp_call v3** (emitted in generated.h): host_call intercept first, then
  `pc=address; if(!dolrecomp_enter)return 0; while(budget>0 && !exception && dolrecomp_enter){}
  return 1;` — rc=0 means "not enterable, pc unchanged" (chassis must make progress another
  way); rc=1 means it executed ≥1 block.
- **chunks/dolrecomp_tables.c** (NEW emitted file, globbed by both builds): chunk fn table,
  leader bitmaps table, `dolrecomp_chunk_states` (const u8*, NULL ⇒ all runnable),
  `dolrecomp_pc_enterable(u32)`, `dolrecomp_enter(CPUState*)`. Chassis binds its
  m_chunk_state array (values 0/1/2; CHUNK_VERIFIED==1 == the `==1` check in
  DOLRECOMP_CHUNK_OK) via dlsym at LoadModule ⇒ cross-chunk native transfers respect SMC
  demotion/verification: transfer into a !=VERIFIED chunk returns to the dispatcher, which
  verifies (one round-trip per first touch per chunk) or demotes.
- **Inline runtime in emitted header:** dolrecomp_read/write{8,16,32,64} flat-RAM fast paths
  (two-window translate exactly mirroring cpu.c translate_addr incl. end-straddles; writes
  divert to out-of-line mem_write* when `ppc_mem_journal_active` — new exported flag set by
  ppc_set_mem_write_journal, present in BOTH cpu.c copies); `dolrecomp_fp_available` inline
  gate (reads new exported `ppc_lazy_fp_enabled`); FP gate emitted only for the FIRST FPU op
  of each accounting block and re-armed after mtmsr (MSR.FP cannot change mid-block otherwise;
  re-entry lands on leaders which re-gate). DOLRECOMP_ENTER/DOLRECOMP_CHUNK_OK default no-op
  macros in the boilerplate, overridden by the split emitter's dispatch helpers — this keeps
  the oracle/diff/codegen single-function harnesses compiling and running with v2 semantics
  (budget 0).
- **Chassis run loop v3:** SyncIn preloads (PreloadBudget: budget=max(ppc.downcount,1), resets
  host_depth, records m_burst_preload); dispatch chains internally for the whole slice
  (~1 dispatch per slice now); FlushBudget computes consumed and charges ppc.downcount;
  min-1-progress on rc!=0&&consumed==0; rc==0 → break to the outer loop (interpreter path
  steps non-leader PCs). DispatchableAt = ChunkIndexOf+Verify + enterable(); FastDispatchableAt
  = enterable() alone. Lockstep: charge passed in (preload−remaining), shadow steps EXACTLY
  native_charge cycles then requires pc==end_pc (the old non-sequential-arrival stop and
  loop-header/undercharge machinery are REMOVED — end_pc can be passed many times mid-dispatch
  now); cycle mismatch at a reached end_pc is reported as ` cyc:N=,I=`; STEPCAP default 1<<18.
  VerboseCounters mask 0x3FFFFF→0xFFF (dispatches are ~5000x bigger).
- **Hook abort mechanism (SUSPECT — see §4):** HookExternalRead/Write/Read32/Write32 and the
  fallback tail zero `cpu->downcount` when ppc.Exceptions has SYNC bits; OnICacheInvalidate
  also zeroes it (so chained execution returns at the next check, block-granular, for SMC
  demotion). **KNOWN-DEFECTIVE BY DESIGN: writing 0 over a NEGATIVE remaining budget RAISES it
  and corrupts consumed-accounting (and over a positive one, inflates consumed). Replace with a
  host-abort sentinel bit in `ctx->exception` (e.g. 0x80000000, a bit ppc_take_exception never
  sets): all emitted call-loops and dolrecomp_call already abort on ctx->exception!=0 within
  one block; the run loop must strip the sentinel without counting it as a guest exception.
  Do this REGARDLESS of what the lockstep probe says.**

## 2. What is DONE and VERIFIED GREEN

- **DolRecomp (branch `main`; was on `pr-oracle-suite` — SWITCHED, see §6):**
  emitter.c fully rewritten to v3 (EmitCtx threading, refunds via emit_exc_return on every
  mid-block exception exit: FP gate, lfs/lfd/stfs/stfd, psq l/st, lswx illegal, stsw, lwarx,
  stwcx, dcbz_l, mftb/mfspr-TB, tlbie, eciwx/ecowx, tw/twi), emit_compute_leaders (global),
  emit_function_ex(+leader bitmap emission `dolrecomp_leaders_%08X`), header inline runtime,
  emitter.h new API, main.c two-pass restructure (decode ALL sections → global leaders → all
  chunk jobs with full DolRecompChunkLayout → dolrecomp_tables.c + v3 dispatch helpers; the
  range-guard text shape `address >= 0x...u && address < 0x...u` is PRESERVED in
  dolrecomp_covered because chassis-module/gen_module_tables.py regex-parses it), cpu.h ABI v3,
  cpu.c journal+flag+lazy-flag export.
  **ctest 10/10 PASS** (incl. emitted_diff, dedicated_diff, codegen_compile, pc_reference).
  **Oracle: host_diff 239 cases, 6 known stswi/stswx XFAIL, 0 unexpected, 0 XPASS; dedicated
  26/0** (`cd DolRecomp/tests/oracle && make diff && make dedicated`).
- **GXRuntime:** include/core/cpu.h v3 (version 3, budget doc, host_depth, MAX_HOST_DEPTH,
  ppc_lazy_fp_enabled + PPCMemWriteJournal typedef + ppc_set_mem_write_journal +
  ppc_mem_journal_active declared), src/core/cpu.c (flag in setter, lazy flag exported),
  tests/runtime_tests.c ABI guards updated to v3 (+host_depth tail assert).
- **StrikersRecomp:** regen done (163 chunks + dolrecomp_tables.c = 164 files; nested
  generated/generated/ moved up); fix_generated.py updated (recognizes v3 → no-op; fails loudly
  on unknown shapes; docstring updated) and run ("Applied 0"); CMakeLists accepts the v3 marker
  (`int dolrecomp_enter(CPUState* ctx);`) or the old fixed marker; chassis-module/module_export.c
  exports `.enterable = chassis_enterable` (dolrecomp_pc_enterable).
- **dolphin-chassis (branch `public-main` + prior uncommitted naming-revert WIP kept):**
  StaticRecompABI.h v3 (version 3, enterable field, dispatch contract re-documented),
  dolrecomp/cpu.h re-vendored from GXRuntime, StaticRecompCore.{h,cpp} v3 (everything in §1),
  + TEMPORARY diagnostics (see §5).
- **Builds:** module (ThinLTO Release) ✓, chassis nogui ✓, standalone StrikersRecomp GUI build
  ✓ (0 errors; boot smoke NOT run yet).
- **Runtime evidence:** Strikers BOOTS on chassis v3 and runs (health screen → menus; movie
  plays; game progresses; ~1 dispatch per CoreTiming slice ≈ 24k cycles/dispatch as designed).
  Capped: charged/wall = 487M cycles/s = exactly 100% speed through boot. **Uncapped
  (EmulationSpeed=0) 60s boot window: 942M cycles/s = 194%** — on the WORST scene (boot/movie:
  10.9M interpreter fallback steps from the SMC'd VMBASE chunk + THP + GPU sharing the thread).
  Covered-code-dominant scenes (the match) should be far higher — NOT yet measured (blocked on
  lockstep + probe-free rebuild).
- **Microbench** (scratchpad/microbench/bench.c): v3 shape 3.77G guest-inst/s vs 2.19G for the
  v2 shape with a zero-cost mock dispatcher (real dispatcher is ~10x heavier), 60k vs 300M
  dispatches; disassembly confirms full register allocation (clang even auto-vectorized the
  paired GPR updates). JIT-at-379% ≈ 1.8G guest-cycles/s ⇒ ceiling is reachable.

## 3. What is RED / NOT DONE

- **LOCKSTEP RED: 23 CTRLFLOW divergences** in a 45s boot window (STATICRECOMP_LOCKSTEP=1,
  no cap). Fully deterministic. THE arc gate — nothing ships until 0. Investigation state in §4.
- Gates not run: prime invariant (module-less Melee + StaticRecompModule=False toggle),
  standalone boot smoke, savestate round-trip, match-scene perf A/B vs CPUCore=1.
- GXRuntime `gxruntime_tests` SEGFAULTS at dyld startup in a fresh `build-cpu` dir — **verified
  PRE-EXISTING via git-stash test (exit 138 on the committed tree too)**; program-66's in-flight
  graphics state, NOT this arc's ABI edits (cpu semantics are covered far deeper by the
  DolRecomp oracle suites + lockstep). Don't chase it here; note for program 66.
- Baseline overlay screenshots failed (empty `wid`): one confirmed cause is capturing after the
  window died; winlist + the grep/sed pipeline DID work against a live window. For JIT-side
  Max% you need the overlay (no counters); suggested protocol: SIGUSR1 savestate at a match on
  a capped chassis run, then SIGUSR2-load into uncapped runs of BOTH cores; read chassis speed
  from charged/wall and JIT from an overlay screenshot.

## 4. The lockstep investigation — findings, dead ends, and the exact next probe

**Symptom:** all reports are CTRLFLOW (shadow ends at a different pc than native's end_pc).
Deterministic across runs. Examples (with the temp `first_arrival` diagnostic = shadow cycles at
first pc==end_pc):
- `#1 entry=0x80141D74 end=0x802505E4 N_cyc=15415 first_arrival=3136`
- `#10 entry=0x801DBDE0 end=0x801DC354 N_cyc=8221 first_arrival=4`
- CAP=8 repro: `entry=0x8024E758 end=0x8024C18C steps=35 N_cyc=45 I_cyc=45 first_arrival=-1`

**Facts established (each by direct experiment):**
1. `I_cyc == N_cyc` in reports is MEANINGLESS — it is the shadow loop's own stop condition
   (`while (interp_cycles < native_charge)`). I burned several analysis rounds before seeing
   this. Only I_pc vs N_end and first_arrival carry information.
2. Budget-cap bisect: divergences at CAP=8 (3), CAP=128 (10), CAP=2048 (many) ⇒ NOT purely
   chain-length-dependent; reachable at near-block granularity.
3. Inline-write probe (forced ALL dolrecomp_write* through the journaled slow path): divergences
   IDENTICAL ⇒ inline stores + journaling exonerated.
4. Cycle-cost table verified: chassis PPCTables loads/stores=1 (same as pristine), stmw/lmw=11,
   matches inst_cycle_cost — the 35-step repro's shadow consumption (45 = 34×1 + stmw 11) is
   EXACT table agreement.
5. `ppc_mftb` is a pure read of ctx->timebase (constant per dispatch) and the shadow pins the
   same value — no tb asymmetry.
6. cpu.c NEVER touches downcount (grep) ⇒ only emitted charges (down), emitted refunds (up,
   exception paths only), and CHASSIS code (Preload/Flush/zero-hooks) write it.
7. Breadcrumb probe (printf at labels in chunk_0147): native's CAP=8 repro path is
   `758(dc=8) → 85C(dc=−6) → 8CC(dc=−34) → 930(dc=−49)` — the SAME path the shadow walks, with
   charges 14/28/15/22 exactly as emitted.
8. Shadow trace: stops at 0x8024E8D8 = 3 instructions into the 8CC block, at exactly 45 cycles
   (14+28+3). So the reported charge (45) corresponds to a stop at 8D8, but the reported
   N_end (0x8024C18C) does not: nothing in the chunk references 0x8024C18C; it is almost
   certainly the dispatch-entry LR (the 930 block ends in blr; with a dead budget the chain
   loop ends with pc=lr). A pure charge walk to that blr gives consumed=79 (remaining −71),
   not 45. **The +34 gap (= 12, the 8CC suffix after 8D4, + 22, the whole 930 block) is the
   unexplained positive adjustment.** Candidate mechanisms, in order:
   (a) my hook budget-zeroing writing 0 over a negative remaining (raises it) — the write-hook
       zero-probe caught ZERO firings, BUT its run window was shorter than the breadcrumb run's
       and gather-pipe stores RETURN EARLY before my zero line (so gather stores can never fire
       it — only the MMU-path writes/reads can); the READ-hook zero (lbz slow paths!) was NOT
       probed — the 8CC block is full of lbz via r6 and the 930 block does RAM reads too;
   (b) a spurious emitted refund (no shape found at the arithmetic's required sites — audited);
   (c) m_burst_preload overwritten mid-dispatch by a fallback SyncOut/SyncIn re-preload — but
       fallback marks m_ls_fallback_seen which SKIPS the check entirely, and a report exists;
   (d) something unimagined.
   **The killer detail supporting (a): `lbz` at 0x8024E8D0/8CC reads via r6 — IF r6's target is
   NON-FLAT (external/MMIO), the read goes HookExternalRead, whose tail zeroes downcount when
   ANY stale sync bit is pending — at dc=−37 (i.e. right after the 3rd instruction 8D4… the
   arithmetic fits a zero firing DURING the 8CC block, then 930's charge −22 and the gather
   stores/blr ending the chain at remaining −22… gives consumed 30, not 45 — so plain (a) does
   NOT close the arithmetic either; a zero at exactly dc=−34+... nothing lands on 45 cleanly.
   Hence the probe below is REQUIRED — do not fix blind.**

**THE NEXT ACTION (everything is already in the tree, just run it):** the current
dolphin-chassis build has THREE diagnostics compiled in: `[ret-probe]` (prints
preload/remaining/pc/rc/exc/depth after EVERY dispatch with entry 0x8024E758 — the FIRST line
is the lockstep-checked one), `[zero-probe]` (prints ea/Exceptions/dc when the WRITE-hook zero
fires — extend to the READ hook if silent), and `[bc]` breadcrumbs in the generated chunk.
Run:
```
ninja -C dolphin-chassis/build dolphin-emu-nogui
env STATICRECOMP_MODULE=$PWD/StrikersRecomp/build-chassis-module/gG4QE01_recomp.dylib \
  STATICRECOMP_LOCKSTEP=1 STATICRECOMP_BUDGET_CAP=8 \
  dolphin-chassis/build/Binaries/dolphin-emu-nogui -e "Super Mario Strikers (USA).iso" \
  -u .tools/dolphin/user -v Metal -C Dolphin.Core.CPUCore=6 > /tmp/v3rp.log 2>&1 &
sleep 30; kill -9 %1; grep -E "ret-probe|zero-probe" /tmp/v3rp.log | head; grep "\[bc\]" /tmp/v3rp.log | head
```
The first `[ret-probe]` line's (preload, remaining, pc, exc) reconciles the arithmetic and names
the writer. Then: fix (replace zero-hooks with the ctx->exception host-abort sentinel from §1
regardless; plus whatever the probe names), REGEN generated/ (see §5 — probes are baked into the
current generated tree!), rebuild everything, and re-run lockstep to 0 over boot→match.
(A Bash-tool classifier outage blocked exactly this run at wind-down time; it had flickered
on/off for ~20 minutes.)

## 5. ⚠ TREE CONTAMINATION — throwaway diagnostics currently applied (MUST clean before any measurement/commit)

- `StrikersRecomp/generated/generated.h`: the inline-write PROBE is still applied —
  `if (DOLRECOMP_EXPECT(ppc_mem_journal_active, 0)) { mem_write` was replaced by
  `if (1) { mem_write` (all 4 write sizes) ⇒ ALL stores currently take the slow path.
  **Perf numbers from the current module are therefore UNDERSTATED.**
- `StrikersRecomp/generated/chunks/chunk_0147_text1_8024D6C0.c`: breadcrumb fprintf's +
  `#include <stdio.h>` + `static int bc_on`.
- FIX: regen wipes both: `../DolRecomp/build/dolrecomp -j8 --gamecube generated/main.dol generated/`
  (output nests into `generated/generated/` — move generated.h/generated.c/generated_smc.txt up
  and replace chunks/, then `rm -rf generated/generated`), then `python3 tools/fix_generated.py`
  (expect "Applied 0"), then `ninja -C build-chassis-module`.
- `dolphin-chassis .../StaticRecompCore.cpp`: `[ret-probe]` fprintf block in the run loop
  (hardcoded entry 0x8024E758) and `[zero-probe]` fprintf in HookExternalWrite — REMOVE both
  after diagnosis. `first_arrival` in LockstepCheck + the CTRLFLOW report field — small, keep
  or drop at taste. `STATICRECOMP_BUDGET_CAP` env knob in PreloadBudget — genuinely useful
  diagnostic, consider keeping documented.
- NOTHING from this arc is committed yet, in any repo. Commit per-repo only after lockstep=0.

## 6. Repo/branch state (as found + as changed)

- **DolRecomp**: was checked out on `pr-oracle-suite` (slim upstream-PR branch, program 67);
  I SWITCHED to `main` (the program-65 emitter lineage: 689797a + my uncommitted v3 work).
  Restore `pr-oracle-suite` for program 67 when needed. Uncommitted: src/backend/emitter.{c,h},
  src/main.c, src/core/cpu.{h,c}.
- **dolphin-chassis**: branch `public-main` (= arc-5 + publication commit 65cdd1af2f); carried
  pre-existing uncommitted DolRuntime→GXRuntime naming reverts (kept intentionally, they match
  the local GXRuntime headers); my uncommitted v3 changes on top (ABI header, vendored cpu.h,
  core .h/.cpp).
- **GXRuntime**: `public-main` @ f83d258 with LOTS of pre-existing program-66 graphics WIP
  (untouched) + my cpu.h/cpu.c/runtime_tests edits. Pre-existing gxruntime_tests dyld segfault
  (stash-verified). New build dir `build-cpu` created (AURORA=OFF) for the CPU suite.
- **StrikersRecomp**: uncommitted: CMakeLists.txt (v3 marker guard), tools/fix_generated.py,
  chassis-module/module_export.c, generated/* (REGENERATED + probe contamination §5).
- Baseline artifacts: scratchpad/baseline/ (chassis.log with counters; jit.log; screenshots
  failed); microbench in scratchpad/microbench/; run logs /tmp/v3*.log (smoke, uncapped, ls,
  trace×3, probes, caps).

## 7. Mistakes made / time wasted (so the next agent doesn't repeat them)

1. **Read `I_cyc == N_cyc` as "cycle-exact agreement" for several loops** when it is forced by
   the shadow's own stop condition. Always ask what a metric CAN say before reading it.
2. **Misread a `uniq -c`-style pc summary as a temporal sequence** ("shadow parks in idle loop
   at the END") — the idle-loop lines were mid-stream; the real tail was elsewhere. Print
   ordered traces, not histograms, when order matters.
3. **Ran the zero-probe with a shorter window (20s) than the run that exhibits the behavior
   (25s+) and initially treated silence as exoneration.** Probe windows must dominate the
   repro window.
4. Several hypothesis paragraphs without a new tool result (the CLAUDE.md circuit breaker);
   the productive turns were exactly the mechanical probes: first_arrival diagnostic, CAP
   bisect, write-slow probe, breadcrumbs, per-instruction shadow trace. Instrument early.
5. Wasted a baseline A/B slot on screenshots that captured after process kill (empty wid) —
   and the arc-4 numbers were already adequate as the reference.
6. Started a microbenchmark file and typo'd it mid-write (harmless, rewrote) — write whole
   files in one pass.
7. Small detour: DolRecomp checkout was on the WRONG BRANCH for this program (publication
   branch without the downcount emitter) — cost ~10 minutes of "where did the emitter go".
   Check `git branch --show-current` in every repo at session start when work spans repos.
8. The Bash-tool classifier outage (intermittent for ~20+ min at the end) blocked the decisive
   probe run; I kept making read-only progress (arithmetic bounding of the defect) — right call,
   but I should have scheduled the run as a background task during an earlier window.

## 8. Durable facts learned (fold into KNOWLEDGE at arc end)

- Dolphin `Interpreter::SingleStepInner` has a SINGLE exit charging `opinfo->num_cycles` on
  every path — faulting instructions (FPU-unavail, DSI, program) ARE charged. Refund model must
  charge the faulter.
- Chassis/pristine PPCTables: integer/load/store ops = 1 cycle (loads/stores are NOT 2),
  lmw/stmw=11, mul 3-5, divw 40, sc/rfi 2 — inst_cycle_cost mirrors it correctly.
- `bcl` semantics as emitted (LR set only when TAKEN) are oracle-clean for this corpus — do not
  "fix" to unconditional-LR without evidence.
- GPFifo gather writes take the HookExternalWrite EARLY-RETURN branch — any tail code in that
  hook never sees them.
- The get-pc idiom (`bcl 20,31,$+4` / bl-to-next) needs the no-call fallthrough special case —
  emitted as just `lr=R`.
- clang at -O3 register-allocates whole leader-blocks (even vectorizes adjacent ctx->gpr
  updates) once per-instruction case labels are gone; block-boundary spills remain (leaders are
  switch-reachable joins) — same flush model as Dolphin's JIT blocks.
- A dispatch now = a whole CoreTiming slice (~20k cycles): mftb staleness bound unchanged
  (refreshed per SyncIn), EXT_INT latency unchanged (slice-granular by construction).
- `winlist` output format `wid=N owner=[dolphin-emu-nogui] ...` and the sed pipeline work; the
  window must still be alive.
- ~48 emitted-code files regen in seconds; module ThinLTO rebuild ~1-2 min; chassis
  StaticRecompCore-only rebuild ~30s.

## 9. Definition of done for this arc (unchanged)

1. Lockstep boot→match **0 DIVERGE** (the product bit-exactness guarantee).
2. DolRecomp ctest 10/10; oracle 239/6-known-XFAIL/0-unexpected; dedicated 26/0 (re-run after
   fixes; all green as of this wind-down).
3. Prime invariant: module-less arbitrary ISO identical to stock; toggle-off forces interpreter.
4. Standalone StrikersRecomp builds AND boots (budget-0 degradation proof).
5. Perf: same-scene A/B vs CPUCore=1 — chassis ≥ JIT Max (379% floor is the user's MINIMUM;
   the goal is FASTER). Measure with a clean (probe-free) module, attract-match or
   savestate-pinned scene, uncapped; judge by charged/wall vs 486M (chassis) and overlay (JIT).
6. Clean the §5 contamination, commit per repo, update goals.json G016 + KNOWLEDGE
   (dolphin-chassis.md arc-6 section), restore DolRecomp branch expectations for program 67.
