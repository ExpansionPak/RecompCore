# CHASSIS.md — fork maintenance notes

RecompCore is a deliberately thin fork of [Dolphin](https://github.com/dolphin-emu/dolphin):
the "chassis" for static-recompilation CPU modules. This file documents the seam between
the fork and upstream, the sync policy, and the build/packaging recipes.

- **Upstream base commit:** `1ccbcaa04a95a5807d92429bf35598da345a3f16` ("Merge branch 'release-prep-2606'")
- **License:** GPLv2+ (unchanged). Distribution model: chassis + per-game native modules;
  the user provides the ISO.

## What this fork adds

`PowerPC::CPUCore::StaticRecomp` — a CPU core (`Source/Core/Core/PowerPC/StaticRecomp/`)
that executes statically recompiled per-game native code (DolRecomp output) when the
current PC is covered by a loaded per-game module, and falls back transparently to
Dolphin's interpreter for everything else (RELs, SMC, uncovered code).

**Prime invariant:** with no module present, this fork behaves exactly like stock
Dolphin for any ISO. With a module present, worst case is demotion to the interpreter —
slower, never broken.

Module autoload: on boot with the StaticRecomp core selected, the chassis looks for
`g<GAMEID>_recomp.<dylib|so|dll>` in `<UserDir>/StaticRecompModules/` (override with the
`STATICRECOMP_MODULE` environment variable pointing at an explicit module path).

Build (no Qt; tested on macOS, `-v Metal` is the macOS video backend — pick your platform's):
```sh
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DUSE_MGBA=OFF
ninja -C build dolphin-emu-nogui
./build/Binaries/dolphin-emu-nogui -e <iso> -u <userdir> -v Metal -C Dolphin.Core.CPUCore=6
```

## Config toggle

`Main.Core.StaticRecompModule` (bool, default **true**). With the StaticRecomp core
selected, `false` runs the core interpreter-only — the **prime-invariant path** — without
moving or deleting the module file. Command line:
```sh
-C Dolphin.Core.StaticRecompModule=False   # force interpreter-only (A/B vs the module)
```
Every other core ignores it. This is the product kill-switch and the on-demand invariant.

## Packaging a per-game module (one command)

```sh
StrikersRecomp/tools/package_module.sh [USER_DIR]
```
Builds `gG4QE01_recomp.<dylib|so>` (ThinLTO, Release) and installs it into
`<USER_DIR>/StaticRecompModules/`, where the chassis autoloads it by disc ID. Default
`USER_DIR` is `<repo>/.tools/dolphin/user`. After packaging, any G4QE01 ISO runs on the
chassis with just `-C Dolphin.Core.CPUCore=6` (no `STATICRECOMP_MODULE` env).

Module perf: the module is built with **ThinLTO** so the GXRuntime `cpu.c` helpers
(`mem_read32`/`translate_addr`, the FP & paired-single ops, `ppc_host_call`) inline into
the generated chunk call sites — the dominant per-op cost was out-of-line helper calls
across translation units. Semantics are unchanged (no `-ffast-math`); guarded by the
oracle (239/0) and the lockstep differential (0 divergences).

## Upstream-sync policy

The fork is deliberately **thin and seam-localized** so re-basing onto a newer Dolphin is
cheap. All substantial code is NEW files under `Source/Core/Core/PowerPC/StaticRecomp/`
(never conflict). The only edits to pre-existing upstream files — the "seams" — are:

| File | Seam |
|---|---|
| `Source/Core/Core/PowerPC/PowerPC.h` | `CPUCore::StaticRecomp = 6` enum value |
| `Source/Core/Core/PowerPC/PowerPC.cpp` | `StaticRecomp` in `AvailableCPUCores()` |
| `Source/Core/Core/PowerPC/JitInterface.cpp` | `InitJitCore` case constructs `StaticRecompCore` |
| `Source/Core/Core/Config/MainSettings.{h,cpp}` | `MAIN_STATICRECOMP_MODULE` config toggle |
| `Source/Core/Core/PowerPC/JitCommon/JitCache.h` | protected hook for the SMC-demotion block cache |
| `Source/Core/Core/PowerPC/MMU.cpp` | lockstep write-suppress / read-replay / RAM-journal sinks (guarded, inert when off) |
| `Source/Core/Core/PowerPC/Interpreter/Interpreter_SystemRegisters.cpp` | lockstep timebase override (inert when off) |
| `Source/Core/DolphinNoGUI/{MainNoGUI.cpp,Platform.{h,cpp}}` | SIGUSR1/2 savestate triggers for nogui |
| `Source/Core/Core/CMakeLists.txt` | add the StaticRecomp sources |

**Sync procedure:**
1. `git remote add upstream https://github.com/dolphin-emu/dolphin.git` (once); `git fetch upstream`.
2. Rebase our commits onto the target upstream release commit. New files never conflict;
   resolve only the ~10 seam hunks above (each small and localized).
3. Update this file's **Upstream base commit** hash + a dated sync note.
4. Re-vendor `dolrecomp/{cpu,types}.h` only if GXRuntime's `CPUState` layout changed
   (bump `cpu_abi_version`; the loader rejects a mismatched module).
5. **Re-verify before trusting the synced fork** (all must pass):
   - build `dolphin-emu-nogui`;
   - **invariant**: a module-less ISO on `-C Dolphin.Core.CPUCore=6` boots identically to
     stock (native=0, no exception) — e.g. Melee GALE01;
   - oracle `make -C DolRecomp/tests/oracle diff dedicated` → 239/0 unexpected, 26/0;
   - **lockstep** `STATICRECOMP_LOCKSTEP=1` over Strikers boot→match → 0 divergences.

**Cadence:** sync opportunistically (when an upstream fix is needed), pinned to Dolphin
*release* commits — not on a schedule.
