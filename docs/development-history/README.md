# Development history — verbatim agent working documents

RecompCore was built by AI agents (Claude, running in an agent harness) steered by a human,
between **2026-07-06** (program ratified) and **2026-07-08** (published), as "Program 65" of a
wider GameCube static-recompilation effort. This folder publishes the actual internal working
documents from that process, verbatim, as they stood on 2026-07-10.

## What's here

- **`program-65-dolphin-chassis/`** — the program board (`00_PROGRAM.md`), the cold-start
  resume prompt (`01_RESUME_PROMPT.md`), and the per-arc handoff narratives (arcs 1–7),
  each written at the end of the arc it describes.
- **`knowledge/`** — the durable knowledge-base files the agents were required to read at
  session start and append to whenever they learned something a future session shouldn't
  have to rediscover: debugging methodology (`methodology.md`), exact tool recipes
  (`tools.md`), the Dolphin-as-ground-truth oracle workflow (`dolphin-ground-truth.md`),
  the canonical session start/end prompt templates (`prompts.md`), and the knowledge-base
  index (`README.md`).

## How to read it

These are unedited working notes, not documentation: they include local machine paths, dead
ends, mistakes, mid-arc states, and instructions from one agent session to the next. For
documentation of the current codebase, see `CHASSIS.md` at the repo root and
`module-template/README.md`.

Timeline receipts, as recorded at the time:

- 2026-07-06 — program ratified; arc 1 boots the skeleton core the same day.
- 2026-07-07/08 — arc 4: lockstep differential harness (every native block re-executed on
  Dolphin's interpreter and diffed) reaches **0 architectural divergences over a
  24.3-billion-dispatch boot→match run** of Super Mario Strikers at ~99.9% native execution.
- 2026-07-08 — published as RecompCore.
- 2026-07-08 — arc 5: product hardening; lockstep re-validation clean over 37.7B dispatches.

**Caveat on arcs 6–7 (2026-07-10):** these are *post-publication* work-in-progress on an
ABI v3 dispatch model (native call chaining, aimed at beating the stock JIT), and the last
handoff captures an unfinished investigation mid-flight, red lockstep results included.
None of that WIP shipped: the published main is module ABI v2 and lockstep-clean. The notes
are included because an honest history includes the part where the work stops mid-stride.
