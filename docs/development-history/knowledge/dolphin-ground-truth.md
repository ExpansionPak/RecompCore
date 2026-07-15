# Dolphin is the ground-truth oracle

**Dolphin is invaluable. Use it before tuning or theorizing around a divergence.** The recomp is the implementation under test; its own output cannot establish intended behavior.

## Two complementary oracles
- **Run the real ISO:** authoritative screen content, transitions, cadence, input response, audio behavior, and genuine guest memory.
- **Read Dolphin source:** authoritative GX/VI/PI/PE/SI/AI/DSP/DVD register behavior, interrupts, timing relationships, and edge cases.

The decomp explains game intent. Aurora explains the backend contract. Dolphin connects both to actual GameCube behavior.

## AI DMA source truth used by Strikers
- `dolphin/Source/Core/Core/HW/SystemTimers.cpp::GetAudioDMACallbackPeriod()` schedules one callback per fixed 32-byte audio DMA unit.
- `dolphin/Source/Core/Core/HW/DSP.cpp::UpdateAudioDMA()` reads exactly 32 bytes, sends 8 stereo frames, advances the address/decrements block count, reloads on zero, and raises `INT_AID`.
- `dolphin/Source/Core/Core/HW/AudioInterface.cpp` makes AIDFR select the 32/48 kHz divisor/mixer rate.
- Dolphin's mixer consumes guest big-endian right/left words and presents host left/right. Strikers' current `mem_read16` plus channel ordering matches this.

These facts rejected the movie-uses-48-kHz and wrong-endianness theories. Do not tune sample rate or chunk size from subjective distortion before checking the runtime can execute enough guest work in real time.

## PI/VI/SI interrupt source truth used by GXRuntime
- `dolphin/Source/Core/Core/HW/ProcessorInterface.{h,cpp}` owns PI cause/mask. `cause & mask` raises the external exception; PI cause writes are write-one-to-clear. Dolphin initializes the reset-released state bit (`INT_CAUSE_RST_BUTTON`) and treats it as guest-visible reset-switch state.
- `dolphin/Source/Core/Core/HW/VideoInterface.cpp::UpdateInterrupts()` raises `INT_CAUSE_VI` only when a VI interrupt register has both `IR_INT` and `IR_MASK` set. For Strikers' DI0 high half at `0xCC002030`, the active bit is `0x8000` and the mask bit is `0x1000`.
- `dolphin/Source/Core/Core/HW/SI/SI.cpp::UpdateInterrupts()` raises `INT_CAUSE_SI` when RDSTINT is set and `RDSTINTMSK` is enabled, or when TCINT is set and `TCINTMSK` is enabled. RDSTINT is derived from per-channel RDST bits in STATUS; COMCSR reads do not clear it. Reading either channel input word clears that channel's RDST bit. GXRuntime's `DolSiDevice` now follows this register/interrupt rule; Strikers' HLE PAD path still supplies controller payloads directly.
- PE finish and DSP/AI sources map through PI bits (`INT_CAUSE_PE_FINISH`, `INT_CAUSE_DSP`) after their device-level status/mask says a source is active. Device status must remain visible until the device-specific ack, not merely until PI dispatch starts.

## Memory-card source truth used by Strikers
- `dolphin/Source/Core/Core/HW/EXI/EXI_DeviceMemoryCard.cpp` identifies Nintendo cards as 4/8/16/32/64/128 Mbit, delays command/transfer completion, and persists through raw-card or GCI-folder backends.
- `dolphin/Source/Core/Core/HW/GCMemcard/GCMemcard.h` fixes the filesystem geometry: `BLOCK_SIZE=0x2000`, five metadata blocks, and 127 directory entries. `MbitToFreeBlocks(4)` is 59.
- Strikers' linked SDK source in `smstrikers-decomp/src/Dolphin/card` establishes the public boundary: `CARDProbeEx` returns the Mbit size plus sector bytes; create/write sizes are sector-aligned; reads are 512-byte aligned; completion callbacks receive `(channel,result)`.
- High-level CARD HLE may complete storage immediately, but it must preserve asynchronous guest ordering. The API returns first; the completion callback runs from a later dispatch with guest registers restored afterward.

## EXI channel-controller truth used by GXRuntime
- `dolphin/Source/Core/Core/HW/EXI/EXI.cpp::RegisterMMIO()` lays out three channels at 0x14-byte strides. Each channel has STATUS, DMA address, DMA length, CONTROL, and immediate-data registers.
- `EXI_Channel.cpp` initializes EXTINT on channels 0/1 and chip select 1 on channel 1. STATUS writes replace masks/clock/chip select, acknowledge EXIINT/TCINT/EXTINT write-one-to-clear, and preserve ROMDIS only on channel 0.
- CONTROL TSTART describes immediate (`TLEN+1`) or DMA (`DMA_LENGTH`) transfer plus direction. A synchronous device clears TSTART and latches TCINT; a delayed device completes later. EXI/TC/external-change status only raises PI EXI when its corresponding mask is enabled.
- GXRuntime `DolExi` owns this controller boundary and exposes payload transfer as a callback. IPL/RTC/SRAM and memory-card bytes are devices above it, not controller semantics.

## DI controller truth used by GXRuntime
- `dolphin/Source/Core/Core/HW/DVD/DVDInterface.cpp::RegisterMMIO()` defines ten 32-bit GameCube registers at `0xCC006000..0xCC006024`: status, cover, three command words, DMA address/length/control, immediate data, and read-only config.
- CONFIG resets to 1 (bootrom descrambler disabled). DMA address masks top bits and aligns to 32 bytes (`& 0x03FFFFE0`); DMA length aligns to 32 bytes. CONTROL retains only TSTART/DMA/RW.
- Status and cover interrupt bits are independently mask-gated and write-one-to-clear. Successful DMA completion advances the address, consumes length, clears TSTART, and latches TCINT; command error latches DEINT; abort latches BRKINT; a cover transition latches CVRINT.
- GXRuntime `DolDi` owns the controller and exposes command payload through `DolDiCommandFn`. Strikers' high-level DVD service remains the command implementation because its generated client only reads DI_CONFIG.

## Required workflow
1. Reproduce in Strikers with a clean environment.
2. Boot the same sequence in Dolphin. Let the user drive input-sensitive paths.
3. Capture the genuine visual/audio timing or inspect the exact MEM1 value with `-d`.
4. Compare the same frame boundary, address, and byte order in the recomp.
5. Read the relevant Dolphin hardware implementation before adding an HLE shortcut.
6. Record whether the fix belongs to GXRuntime, the Aurora backend, DolRecomp output, or Strikers policy.

## Isolation oracles: synthetic DOLs and FIFO logs
Do not restrict Dolphin comparison to full retail-game runs:
- A small open-source/handwritten GameCube DOL can write structured results into a known memory block. Run it in Dolphin and through DolRecomp+GXRuntime to isolate PPC, boot, MMIO, interrupt, and timing behavior.
- For DolRecomp emitted-code conformance, Dolphin is the right execution oracle but Dolphin's in-tree unit tests are not the right driver: they mostly cover host-side/JIT helpers and assembler encoding, not arbitrary raw Gekko instructions as hardware goldens. Build a producer DOL and report full-state results through a machine-readable channel such as USB-Gecko/TCP or a `dolphin-emu/hwtests`-style socket, then import those goldens into `tests/diff`.
- `dolphin-emu/hwtests` is a useful transport pattern, not a complete solution: it already builds devkitPPC/libogc hardware tests and reports over TCP, but it does not generate raw opcode probes, invoke DolRecomp's emitter, compare emitted `CPUState`, or catalog known-vs-unknown DolRecomp defects. Reuse the output-channel idea; keep the DolRecomp differential in the owning test suite.
- Use Dolphin Interpreter64 (`Dolphin.Core.CPUCore=0`) as the authoritative opcode oracle. JITARM64 is useful as an optional drift check, but it can differ from Interpreter64 and must not define expected PPC/Gekko behavior.
- DolRecomp's completed oracle pipeline is producer-DOL + Interpreter64 capture + emitted-C `CPUState` diff. The fallthrough trampoline owns ordinary/fuzz cases; a separate emitted-control target owns branch/system/cache/external-control forms that cannot safely return through the trampoline.
- Dolphin FIFO logs isolate graphics from CPU/game execution. Dolphin's FifoCI replays recorded CPU-to-GPU traffic, dumps frames, and compares pixels specifically to catch renderer regressions.

Use libogc as an open-source way to build stimuli, not as the authority for retail Nintendo SDK ABI or hardware behavior. Dolphin execution/source remains the oracle. Keep probe sources and expected structured results versioned with the owning product test suite; `runtimeharness` only documents the recipe.

## Evidence hierarchy
1. Real ISO running in Dolphin at the relevant sequence.
2. Dolphin debugger memory/watchpoints and hardware source.
3. Recomp binary evidence: `lldb`, `sample`, screenshots, focused logs.
4. Decomp source and symbols.
5. Static reasoning.

Lower levels design experiments; they do not overrule higher-level observations.

## Movie caution
`.tools/dolphin/dolphin_fe.sh` loads a Front End save state and bypasses the intro movie. Scripted key timing also skipped/missed it. For THP work, launch a normal boot with `-d`, keep Dolphin open, and let the user drive.

The user-driven Dolphin movie remains the quality bar. The recomp's former movie-only distortion/microstutter was fixed by constant-time generated chunk dispatch, not by changing AID behavior; the user confirmed the final clean movie "sounds great." Keep using a normal Dolphin boot for future movie regressions, not the FE save-state helper.
