# ARC 7 HANDOFF — maximum game-agnostic static-recomp performance (2026-07-10)

## Read this first

We need help from a smarter performance/compiler agent.

The product goal is not merely "faster than before." It is the highest performance a
game-agnostic static PowerPC recompilation pipeline can reasonably deliver, ideally matching or
beating Dolphin's JITARM64 on covered code. Correctness remains mandatory, but **do not begin by
returning to the old lockstep investigation**. The user explicitly asked us to find the best
architecture first. Only return to lockstep if evidence says the winning architecture is the
previous lockstep-dependent path.

Current best static result on the pinned Strikers state is about **278%**, while Dolphin JITARM64
on the exact defocused/null-renderer setup reaches about **861%**. The remaining gap is roughly
**3.1x**. Conventional compiler tuning, helper rearrangement, and PGO did not close it.

The next agent's primary question is:

> How should DolRecomp represent and emit whole-function or region code so guest GPR/FPR/CR state
> stays in host SSA values/registers across instructions, loops, and ordinary memory operations,
> while materializing architectural state only at externally observable exits—and while remaining
> game-agnostic and exactly correct?

Please challenge our conclusion if it is wrong. We want the best solution, not confirmation.

---

## 1. Non-negotiable constraints

- Game-agnostic. No Strikers-specific patches, hardcoded PCs, hand-selected hot functions, or
  per-game semantic assumptions. A symbol map may improve function boundaries, but correctness
  and the architecture cannot depend on one game's symbols.
- Static recompilation quality is the goal. Do not disguise interpreter/JIT work as static recomp.
- Preserve exact guest semantics: exceptions, MMIO, SMC demotion, time base, cycle accounting,
  paired-single behavior, FP conversion/rounding, and observable CPU state.
- Benchmark with the emulator window **defocused**. When Dolphin is focused, this machine can
  throttle/cap at exactly 100%, producing misleading results. Focus Finder, Code, or another app
  before measuring.
- Use `EmulationSpeed=0`, Null renderer, the same savestate, and the CoreTiming benchmark described
  below. Do not compare different boot windows or rely on the UI speed display.
- Optimize the reusable pipeline, not this one generated module.
- The working trees contain unrelated user work. Do not clean, reset, or overwrite it.

---

## 2. Where we started

The pre-v3 generated code was effectively interpreter-shaped:

1. Every guest instruction had a `case`, a label, and a `ctx->pc` store.
2. Every back edge and guest call/return went through the cross-dylib dispatcher.
3. Memory and FP gates used out-of-line helpers at very high frequency.
4. Because every instruction was a possible externally entered join point, Clang kept the entire
   guest context coherent in memory instead of allocating guest values across a region.

This was diagnosed in source and assembly. A small shape microbenchmark showed the opportunity:
the leader-only/native-loop form achieved about 3.77G guest instructions/s versus 2.19G for the
old shape, with roughly 60k versus 300M dispatches. That justified the v3 dispatch redesign.

Prior design rationale is in:

- `2026-07-10_arc6_design_jit-beating-dispatch.md`
- `2026-07-10_arc6_HANDOFF_ALL.md`

Those documents include historical lockstep details and temporary-probe warnings. They are useful
context, but they are not instructions to restart that investigation.

---

## 3. Successful architectural work already in the tree

### 3.1 ABI v3 and native control-flow chaining

The current uncommitted DolRecomp/dolphin-chassis work implements:

- positive preloaded cycle budgets in `CPUState.downcount`;
- leader-only dispatch entries and accounting;
- native local branches/back edges;
- guarded host-stack calls for direct guest calls;
- iterative native re-entry for indirect/non-local control flow;
- exact suffix refunds for mid-block exception exits;
- leader bitmaps and an `enterable(pc)` module API;
- SMC-aware chunk state guards;
- inline flat-RAM fast paths with journal/MMIO fallback;
- once-per-accounting-block lazy-FP availability gates;
- O(1) dispatch rather than a linear or giant per-instruction switch path.

This was a large real improvement. It eliminated dispatcher churn as the dominant problem. Do not
throw it away casually.

### 3.2 Function-map backend

We added `--function-map symbols.txt` for DOL inputs. It:

- parses CodeWarrior-style sized function symbols;
- sorts executable DOL sections and rejects wrap/overlap;
- produces function-oriented native units rather than fixed 16 KiB-only ownership;
- emits a dense address-to-owner table for O(1) dynamic entry;
- uses `u16` owner indexes through exactly 65,536 functions and `u32` above that boundary;
- preserves fallback coverage for gaps/side entries;
- propagates generation errors instead of silently producing incomplete output.

The tests cover malformed maps, section ordering/overlap, side entries, and the 65,536/65,537 owner
width boundary. Current DolRecomp test result: **11/11 pass**.

The accepted function-backend module is preserved at:

`/tmp/gG4QE01-function-baseline.dylib`

- SHA-256: `fee47a4abcc0470c2c99b776fe92bd0257d315a04bdc049bc71cd684eb72c798`
- file size: 33,030,072 bytes
- `__text`: 29,037,840 bytes (`0x1bb1510`)
- expected public exports: three

The final clean rebuild at
`/tmp/gcdecomp-function-module-build/gG4QE01_recomp.dylib` is byte-identical to that baseline.

### 3.3 Reliable, core-neutral benchmark accounting

`dolphin-chassis/Source/Core/Core/CoreTiming.cpp` has an opt-in logger enabled by
`DOLPHIN_BENCHMARK_TB=1`. It reports cumulative emulated global ticks, fake time base, idle ticks,
wall time, CPU frequency, and percentages. It is intentionally in CoreTiming so both JITARM64 and
StaticRecomp are measured by the same clock.

We verified that FakeTB and CoreTiming global-tick accounting agree apart from expected integer
prescaler residue. The static/JIT gap is real; it is not a time-base accounting bug.

Canonical measurements use cumulative `tick_pct`. "Verbose full" and a late-window slice were
also recorded as sanity checks. Idle percentage was stable around 51.9% in the accepted static
runs.

---

## 4. Accepted baseline measurements

Same pinned savestate, Null renderer, uncapped, and window explicitly defocused:

| Run | Canonical cumulative | Verbose/full | Late window | Idle |
|---|---:|---:|---:|---:|
| Static A1 | 278.133% | 277.992% | 289.548% | ~51.91% |
| Static A2 | 278.240% | 278.348% | 290.124% | ~51.95% |
| JITARM64 | ~861.4% | — | — | same pinned state |

Host load later became depressed/noisy, so later experiments were screened with immediate
baseline reversals where possible. Do not compare a depressed 266% run directly against the older
healthy 278% result without an adjacent control.

The fact that a focused window sometimes lands at a hard 100% is environmental throttling/capping,
not a compiler result.

---

## 5. Experiments performed and what each taught us

### 5.1 v3 leader-only/native-chain emitter — KEEP

This removed the worst dispatch and per-instruction synchronization path. It is the foundation of
the accepted backend. It demonstrated that architecture matters much more than small peepholes.

However, generated instructions still commonly read/write fields of `CPUState`, and calls to
runtime helpers remain optimization barriers. Function-shaped C alone does not guarantee that
Clang can retain the guest register file as SSA values across those barriers.

### 5.2 Native CPU tuning (`-mcpu`) — incorporated, not the missing 3x

The module build supports native CPU tuning (Apple M4 detection maps to `-mcpu=apple-m4`) without
`-ffast-math` or semantic relaxation. This is a reasonable default for a local machine-code cache,
but it did not change the architectural diagnosis.

### 5.3 Symbol-guided function boundaries — KEEP as an optional precision input

The function backend improves control-flow scope and ownership and is game-agnostic as a feature:
maps are inputs, not hardcoded logic. It produced the stable ~278% accepted result. It still did not
cause C/LLVM to give us JIT-like explicit register residency.

Do not assume symbols are complete or authoritative. A better backend must work conservatively
without them and can use them when present.

### 5.4 Generated inline `lfs/stfs` fast paths — REJECTED

We tried expanding single-precision FP load/store fast paths directly into generated call sites.

- `__text` grew by about 8.34 MiB / 28.7%.
- accepted baseline: 278.13–278.24%
- candidate: 276.39%

The extra instruction-cache footprint outweighed any removed call overhead. The generated inline
path was removed.

### 5.5 Compact hot/cold runtime helper split — REJECTED/REVERTED

We then kept the call sites compact and split `lfs/stfs` helpers into small hot flat-RAM bodies plus
cold slow paths. Approximate hot body sizes fell from 496/452 bytes to 296/236 bytes, with almost no
whole-module size change.

Under the later depressed host condition:

- candidate canonical: 266.611%
- immediate accepted-baseline reversal: 265.765%

That ~0.3% difference is noise, not a defensible gain. The source changes were reverted in both:

- `DolRecomp/src/core/cpu.c`
- `GXRuntime/src/core/cpu.c`

The final accepted module contains no `ppc_lfs_op_slow`/`ppc_stfs_op_slow` symbols.

### 5.6 Full LTO — REJECTED as a performance default

We added a reusable `DOLRECOMP_LTO_MODE={thin,full,off}` build option to both module templates.
FullLTO candidate:

- path: `/tmp/gcdecomp-function-module-full-build/gG4QE01_recomp.dylib`
- size: 31,758,424 bytes
- `__text`: `0x1a79d9c`, about 1.27 MiB smaller than ThinLTO
- quick canonical screen: 275.919%
- quick verbose screen: 277.722%

It is essentially tied with the healthy ~278% ThinLTO baseline and showed no repeatable speed win.
Keep the selectable mode for future evidence, but ThinLTO remains accepted.

### 5.7 Instrumented PGO — REJECTED

We built an instrumented ThinLTO module, trained it for seven seconds on the exact pinned state,
merged `/tmp/gcdecomp-pgo.profdata` (~10 MiB), and built a profile-use module:

- path: `/tmp/gcdecomp-pgo-use-build/gG4QE01_recomp.dylib`
- size: 30,408,440 bytes
- `__text`: `0x19f99c8`, about 2.62 MiB smaller than ThinLTO
- quick canonical screen: 273.004%
- quick verbose screen: 271.597%

PGO improved layout/size but was neutral-to-negative for throughput. A seven-second single-state
profile is not a production-quality universal training corpus, but the experiment was sufficient
to reject PGO as the missing order-of-magnitude mechanism. It also risks becoming game-specific if
used carelessly.

### 5.8 Lockstep/accounting hypotheses — not the current performance lead

Earlier work deeply investigated cycle refunds, exception exits, hook aborts, and native-vs-shadow
control flow. That matters for correctness, and the old handoff contains an exact repro. But the
new core-neutral clock comparison proves the current 3.1x performance gap is not an accounting
illusion. Do not spend the next arc polishing lockstep before selecting the winning code-generation
architecture.

---

## 6. Research that informed the present conclusion

We examined comparable recompilers and compiler tooling rather than assuming more C inlining would
solve this:

- XenonRecomp reports its largest improvements from local-variable/register optimizations and
  describes substantial code-size and frame-time reductions. This is the closest direct clue:
  <https://github.com/hedge-dev/XenonRecomp>
- N64Recomp is useful for region/function emission and static recompilation structure:
  <https://github.com/N64Recomp/N64Recomp>
- LLVM ThinLTO explains why ThinLTO gives scalable interprocedural optimization but does not create
  an emulator-specific architectural-state SSA model for us:
  <https://clang.llvm.org/docs/ThinLTO.html>
- Clang PGO documentation covers the instrumentation/use pipeline we screened:
  <https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization>

Our inference from source, generated assembly, code size, and the experiments above is that the
remaining dominant issue is **architectural register residency/value propagation**, not dispatch,
LTO selection, or individual helper-call overhead.

---

## 7. What we believe the next architecture should explore

This is a hypothesis, not a mandated implementation. A smarter agent should validate or replace it.

### Candidate: explicit guest-state SSA/value cache

Build a game-agnostic intermediate representation or a disciplined generated-C value-cache layer
that:

1. Loads live-in guest GPR/FPR/CR/XER/LR/CTR values into named host temporaries at region entry.
2. Represents instruction outputs as SSA-like values instead of immediate `ctx->field` stores.
3. Carries values through local branches and loops, with phi/merge handling at CFG joins.
4. Models helper effects precisely. A RAM fast path should not clobber the entire guest register
   cache; MMIO/exception slow paths may require a selective or full state materialization.
5. Spills only values required by an observable exit: exception, MMIO callback, indirect transfer,
   SMC check, dispatcher return, fallback instruction, debugging/lockstep boundary, or ABI call that
   can inspect the context.
6. Reloads only state a slow path can actually change.
7. Separates hot flat-RAM operations from cold semantic paths without duplicating large conversion
   code at every call site.
8. Uses liveness analysis to avoid materializing dead architectural registers.
9. Keeps exact cycle/PC state separately so accounting does not force full register-file coherence.
10. Retains safe side-entry behavior. Side entries can use conservative materialization stubs rather
    than making every instruction a join point.

Possible implementation levels, in increasing control:

- generated C with explicit local value caches plus effect-annotated helpers;
- generated LLVM IR with real basic blocks, phi nodes, attributes, and cold slow-path blocks;
- a compact custom IR lowered to LLVM ORC/object generation ahead of packaging;
- direct AArch64 lowering only if a portable LLVM path demonstrably cannot reach the target.

The ideal experiment is not a full rewrite. Select a representative, game-agnostic instruction
subset/region shape, implement explicit residency, inspect assembly, and run an adjacent A/B. Define
the success gate before expanding it.

### Questions the next agent must answer

1. Is generated LLVM IR materially better than generated C here, or can effect annotations and
   locals make Clang produce equivalent code?
2. What is the minimum safe materialization boundary for RAM helpers, MMIO, exceptions, and chained
   calls?
3. How should FPR paired-single representation and CR fields be cached without semantic drift?
4. How can loops retain values across back edges while still honoring cycle-budget exits?
5. How should unknown indirect targets and symbol-less side entries enter a region?
6. What code-size budget avoids repeating the failed inline-`lfs/stfs` experiment?
7. Can one quantify, using assembly/perf sampling, how much current time is now spills/reloads,
   helper barriers, instruction-cache misses, and remaining dispatch?
8. Would a trace/superblock backend outperform strict function ownership while remaining static and
   game-agnostic?

---

## 8. Fast experimental protocol for the next agent

Do not begin with a multi-hour implementation.

1. Preserve `/tmp/gG4QE01-function-baseline.dylib` and its hash.
2. Profile or disassemble a few representative hot generated functions from the pinned state.
3. Count context loads/stores and spills around ordinary arithmetic plus RAM access.
4. Prototype one architectural shift that directly attacks those counts.
5. Run a short 10-second canonical screen with the window defocused.
6. Immediately reverse to the accepted module if host load looks different.
7. Reject anything within noise or anything that wins only by game-specific selection.
8. Expand only after a clear adjacent win and assembly evidence of the intended mechanism.

Suggested evidence bundle for every candidate:

- canonical `tick_pct`;
- immediate baseline reversal;
- module and `__text` size;
- representative assembly before/after;
- number of guest-context loads/stores or spill instructions in the sampled region;
- correctness tests and which observable boundaries the prototype supports.

---

## 9. Current tree and artifact cautions

All relevant work is uncommitted across nested repositories. The workspace root itself is not one
Git repository. Inspect with `git -C <repo> status --short`.

Relevant modified repositories:

- `DolRecomp`: emitter/ABI/function-map generator and tests.
- `dolphin-chassis`: StaticRecomp ABI/core, CoreTiming benchmark, module template.
- `StrikersRecomp`: module template/table generation and build options.
- `GXRuntime`: CPU ABI/runtime changes coexist with substantial unrelated graphics work. Touch only
  the CPU files when required.
- `runtimeharness`: other programs have active unrelated handoffs/goals and a lock file.

Do not delete or normalize unrelated changes. In particular, most modified GXRuntime graphics files
belong to other work.

The accepted clean ThinLTO rebuild has already been verified byte-for-byte. There is no need to
rebuild it before architectural analysis.

---

## 10. Honest state of the project

We should be satisfied that the current backend is substantially better and that the major
dispatch redesign worked. We should **not** claim maximum performance or parity with JITARM64.

The three rapid architecture-adjacent screens requested by the user—hot/cold helpers, FullLTO, and
PGO—were completed, and none won. Together with the failed generated inline FP path, they are strong
evidence that further flag tuning and helper reshuffling are wheel-spinning.

The next meaningful work is compiler architecture: explicit value residency, effect-aware
materialization, and a CFG/IR capable of carrying guest state across hot regions. Please help us
find the strongest game-agnostic design, prove it first on a bounded experiment, and only then fold
it into the correctness/lockstep program.

---

## 11. New upstream DolRecomp audit (performed after this handoff was written)

We also tested current `ExpansionPak/DolRecomp` upstream rather than assuming our older lineage was
still best:

- local modified base: `689797a519deaafaddd7d4cd956d87827abe8917`
- upstream `main` on 2026-07-10: `f3a129d50a28b4586c559a002e2f7bfc15ecf953`
- upstream was cloned separately at `/tmp/DolRecomp-upstream-audit`; the dirty local tree was not
  touched;
- upstream configured/built successfully and passed **10/10** tests;
- it successfully decoded Strikers' `main.dol` and emitted all **163 chunks**, with zero unknown
  instructions, into `/tmp/gcdecomp-upstream-generated/generated`.

It is **not directly compatible with RecompCore ABI v3**, and it is not presently a faster backend:

- upstream `CPUState` has no ABI version marker, `downcount`, or `host_depth`;
- generated output has no `dolrecomp_pc_enterable`, leader bitmaps, or
  `dolrecomp_chunk_states` binding required by RecompCore's module export/SMC contract;
- upstream uses a linear generated `dolrecomp_find_original` chain over chunk ranges;
- `dolrecomp_run_blocks` calls one generated block at a time;
- its emitter still emits a switch entry and label for **every instruction** and directly accesses
  `ctx->gpr/fpr/cr/...` throughout;
- it lacks our leader-only entries, native local loops/calls, exact preloaded budget chaining,
  exception suffix refunds, inline flat-RAM paths, and O(1) owner dispatch.

Therefore, replacing our DolRecomp checkout with upstream would discard the mechanisms responsible
for the accepted performance improvement and likely regress toward the old interpreter-shaped
backend. It would also fail the RecompCore module contract until ported.

Useful upstream changes should instead be reviewed and selectively ported into our v3 lineage. The
two immediately relevant features are:

- physical-PC alias handling (`f3a129d`);
- host function replacement/original-call support (`b209f0e`).

Neither addresses the remaining 3.1x performance gap. The right long-term integration direction is
to rebase/port upstream's cleaner frontend, REL/RPX support, decoder fixes, and tests around the v3
backend—or upstream the v3 backend itself—without adopting upstream's current per-instruction
dispatch/code-generation shape.
