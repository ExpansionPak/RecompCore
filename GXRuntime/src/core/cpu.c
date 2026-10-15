#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* dladdr, dlinfo (fastmem fault handler) */
#endif
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

/* --- Chassis lockstep journal (debug-only, NOT part of the CPU ABI) ---------
 * Optional callback fired just before every committed RAM write. The
 * dolphin-chassis differential ("lockstep") harness installs it to snapshot
 * pre-images, so it can restore a pre-block memory view before re-running a
 * block on Dolphin's interpreter (correct read-modify-write comparison).
 * NULL and zero-cost unless installed; the chassis resolves the setter by name
 * via dlsym, so its absence simply disables lockstep. `offset` is the RAM byte
 * offset (into cpu->ram) about to be written, `size` the width in bytes. */
PPCMemWriteJournal g_mem_write_journal = NULL;
void* g_mem_write_journal_user = NULL;

#if DOLRECOMP_FASTMEM
/* See the fastmem block in cpu.h. */
u8* g_dolrecomp_fm_base = NULL;
u64 g_dolrecomp_fm_min = 1ull << 32;  /* off until the runtime hands out a base */
int g_dolrecomp_fm_queried = 0;
static CPUState* g_dolrecomp_fm_cpu;
static uintptr_t g_dolrecomp_fm_code_lo, g_dolrecomp_fm_code_hi;

/* A fastmem access that faulted: an address in the view that no BAT maps
   (beyond main RAM, a stray pointer, a page between DBAT updates). Decode
   the faulting instruction - only the forms the cpu.h accessors emit:
   [66] [REX] 8B/89/88 or 0F B6/B7 with a memory ModRM - do the access
   through the checked accessors, exactly as with fastmem off (a RAM alias
   the view has not mapped yet still reaches RAM), update
   the destination register and step over the instruction. regs is the 16
   general registers in encoding order (rax, rcx, rdx, rbx, rsp, rbp, rsi,
   rdi, r8..r15). Returns 0 when this is not one of ours. */
static int dolrecomp_fastmem_emulate(uintptr_t rip, u64* regs[16]) {
    if (rip < g_dolrecomp_fm_code_lo || rip >= g_dolrecomp_fm_code_hi)
        return -1;
    const u8* ip = (const u8*)rip;
    int i = 0, op16 = 0, rex = 0;
    if (ip[i] == 0x66) { op16 = 1; i++; }
    if ((ip[i] & 0xF0) == 0x40) { rex = ip[i]; i++; }
    int store, size, zext32 = 1;
    if (ip[i] == 0x8B) { store = 0; size = (rex & 8) ? 8 : 4; i++; }
    else if (ip[i] == 0x89) { store = 1; size = (rex & 8) ? 8 : (op16 ? 2 : 4); i++; }
    else if (ip[i] == 0x88) { store = 1; size = 1; i++; }
    else if (ip[i] == 0x0F && ip[i + 1] == 0xB6) { store = 0; size = 1; i += 2; }
    else if (ip[i] == 0x0F && ip[i + 1] == 0xB7) { store = 0; size = 2; i += 2; }
    else return -1;
    if (size == 8) zext32 = 0;
    const u8 modrm = ip[i++];
    const int mod = modrm >> 6, rm = modrm & 7;
    const int reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    if (mod == 3)
        return -1;
    u64 ea = 0;
    if (rm == 4) {
        const u8 sib = ip[i++];
        const int scale = sib >> 6;
        const int index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
        const int base = (sib & 7) | ((rex & 1) ? 8 : 0);
        if (index != 4)
            ea += *regs[index] << scale;
        if (mod == 0 && (sib & 7) == 5) {
            int32_t d; memcpy(&d, ip + i, 4); i += 4; ea += (u64)(int64_t)d;
        } else {
            ea += *regs[base];
        }
    } else if (mod == 0 && rm == 5) {
        return -1;  /* RIP-relative: never a guest access */
    } else {
        ea += *regs[rm | ((rex & 1) ? 8 : 0)];
    }
    if (mod == 1) { ea += (u64)(int64_t)(int8_t)ip[i]; i += 1; }
    else if (mod == 2) { int32_t d; memcpy(&d, ip + i, 4); i += 4; ea += (u64)(int64_t)d; }
    const u64 off = ea - (u64)(uintptr_t)g_dolrecomp_fm_base;
    if (off > 0xFFFFFFFFull)
        return -1;
    const u32 guest = (u32)off;
    /* Log at the 1st, 2nd, 4th, 8th... fault: a steady stream here would mean
       a hot region missing from the view (it should be rare stray accesses). */
    static unsigned long long faults;
    const unsigned long long nth = ++faults;
    if ((nth & (nth - 1)) == 0)
        fprintf(stderr, "[fastmem] fault #%llu: %s %08X size %d\n", nth, store ? "write" : "read", guest, size);
    CPUState* cpu = g_dolrecomp_fm_cpu;
    if (store) {
        u64 raw;
        if (size == 1 && !rex && reg >= 4 && reg < 8)
            raw = (*regs[reg - 4] >> 8) & 0xFF;  /* ah, ch, dh, bh */
        else
            raw = *regs[reg];
        u64 value = size == 8 ? bswap64(raw) : size == 4 ? bswap32((u32)raw)
                  : size == 2 ? bswap16((u16)raw) : (raw & 0xFF);
        switch (size) {
        case 1: dolrecomp_mem_write8_slow(cpu, guest, (u8)value); break;
        case 2: dolrecomp_mem_write16_slow(cpu, guest, (u16)value); break;
        case 4: dolrecomp_mem_write32_slow(cpu, guest, (u32)value); break;
        default: dolrecomp_mem_write64_slow(cpu, guest, value); break;
        }
    } else {
        u64 value;
        switch (size) {
        case 1: value = dolrecomp_mem_read8_slow(cpu, guest); break;
        case 2: value = dolrecomp_mem_read16_slow(cpu, guest); break;
        case 4: value = dolrecomp_mem_read32_slow(cpu, guest); break;
        default: value = dolrecomp_mem_read64_slow(cpu, guest); break;
        }
        u64 raw = size == 8 ? bswap64(value) : size == 4 ? bswap32((u32)value)
                : size == 2 ? bswap16((u16)value) : (value & 0xFF);
        *regs[reg] = zext32 ? (u32)raw : raw;
    }
    return i;
}

#if defined(_WIN32)
#include <windows.h>
static PVOID g_dolrecomp_fm_handler;
/* The module can be unloaded while the emulator lives on; do not leave a
   process-wide handler pointing into its unmapped image. */
static void dolrecomp_fastmem_uninstall(void) {
    if (g_dolrecomp_fm_handler)
        RemoveVectoredExceptionHandler(g_dolrecomp_fm_handler);
}
static LONG CALLBACK dolrecomp_fastmem_fault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t fault = (uintptr_t)er->ExceptionInformation[1];
    const uintptr_t base = (uintptr_t)g_dolrecomp_fm_base;
    if (!base || fault < base || fault - base > 0xFFFFFFFFull + 8)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    u64* regs[16] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                     &c->R8,  &c->R9,  &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
    const int len = dolrecomp_fastmem_emulate((uintptr_t)c->Rip, regs);
    if (len < 0)
        return EXCEPTION_CONTINUE_SEARCH;
    c->Rip += (DWORD64)len;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static int dolrecomp_fastmem_install(void) {
    HMODULE self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void*)&dolrecomp_fastmem_query, &self))
        return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const u8*)self + dos->e_lfanew);
    g_dolrecomp_fm_code_lo = (uintptr_t)self;
    g_dolrecomp_fm_code_hi = (uintptr_t)self + nt->OptionalHeader.SizeOfImage;
    g_dolrecomp_fm_handler = AddVectoredExceptionHandler(1, dolrecomp_fastmem_fault);
    /* Unlike a compiler destructor section, the DLL CRT on-exit table runs
       at FreeLibrary with both static and dynamic CRT linkage. */
    if (g_dolrecomp_fm_handler && atexit(dolrecomp_fastmem_uninstall) != 0) {
        dolrecomp_fastmem_uninstall();
        g_dolrecomp_fm_handler = NULL;
    }
    return g_dolrecomp_fm_handler != NULL;
}
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <ucontext.h>
static struct sigaction g_dolrecomp_fm_prev;
static int g_dolrecomp_fm_installed;
static void dolrecomp_fastmem_fault(int sig, siginfo_t* info, void* raw);
/* Unloading the module puts the previous SIGSEGV action back (see the
   Windows branch). */
static void __attribute__((destructor)) dolrecomp_fastmem_uninstall(void) {
    struct sigaction current;
    if (g_dolrecomp_fm_installed && sigaction(SIGSEGV, NULL, &current) == 0 &&
        (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == dolrecomp_fastmem_fault)
        sigaction(SIGSEGV, &g_dolrecomp_fm_prev, NULL);
}
static void dolrecomp_fastmem_fault(int sig, siginfo_t* info, void* raw) {
    ucontext_t* uc = (ucontext_t*)raw;
    const uintptr_t fault = (uintptr_t)info->si_addr;
    const uintptr_t base = (uintptr_t)g_dolrecomp_fm_base;
    if (base && fault >= base && fault - base <= 0xFFFFFFFFull + 8) {
        greg_t* g = uc->uc_mcontext.gregs;
        u64* regs[16] = {(u64*)&g[REG_RAX], (u64*)&g[REG_RCX], (u64*)&g[REG_RDX], (u64*)&g[REG_RBX],
                         (u64*)&g[REG_RSP], (u64*)&g[REG_RBP], (u64*)&g[REG_RSI], (u64*)&g[REG_RDI],
                         (u64*)&g[REG_R8],  (u64*)&g[REG_R9],  (u64*)&g[REG_R10], (u64*)&g[REG_R11],
                         (u64*)&g[REG_R12], (u64*)&g[REG_R13], (u64*)&g[REG_R14], (u64*)&g[REG_R15]};
        const int len = dolrecomp_fastmem_emulate((uintptr_t)g[REG_RIP], regs);
        if (len >= 0) {
            g[REG_RIP] += len;
            return;
        }
    }
    /* Not ours: hand it to whoever was installed before (Dolphin's handler). */
    if (g_dolrecomp_fm_prev.sa_flags & SA_SIGINFO) {
        if (g_dolrecomp_fm_prev.sa_sigaction) { g_dolrecomp_fm_prev.sa_sigaction(sig, info, raw); return; }
    } else if (g_dolrecomp_fm_prev.sa_handler != SIG_DFL && g_dolrecomp_fm_prev.sa_handler != SIG_IGN) {
        g_dolrecomp_fm_prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static int dolrecomp_fastmem_install(void) {
    Dl_info dl;
    if (!dladdr((void*)&dolrecomp_fastmem_query, &dl) || !dl.dli_fbase)
        return 0;
    /* The module's mapped extent: the span of its PT_LOAD segments. */
    struct link_map* lm = NULL;
    void* h = dlopen(dl.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!h)
        return 0;
    if (dlinfo(h, RTLD_DI_LINKMAP, &lm) != 0 || !lm) {
        dlclose(h);
        return 0;
    }
    const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)dl.dli_fbase;
    const ElfW(Phdr)* ph = (const ElfW(Phdr)*)((const u8*)eh + eh->e_phoff);
    uintptr_t hi = 0;
    for (int k = 0; k < eh->e_phnum; k++)
        if (ph[k].p_type == PT_LOAD && lm->l_addr + ph[k].p_vaddr + ph[k].p_memsz > hi)
            hi = lm->l_addr + ph[k].p_vaddr + ph[k].p_memsz;
    g_dolrecomp_fm_code_lo = (uintptr_t)dl.dli_fbase;
    g_dolrecomp_fm_code_hi = hi;
    /* RTLD_NOLOAD still acquires a reference; retaining it would prevent unload. */
    dlclose(h);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = dolrecomp_fastmem_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    g_dolrecomp_fm_installed = sigaction(SIGSEGV, &sa, &g_dolrecomp_fm_prev) == 0;
    return g_dolrecomp_fm_installed;
}
#else
static int dolrecomp_fastmem_install(void) { return 0; }
#endif

void dolrecomp_fastmem_query(CPUState* cpu) {
    g_dolrecomp_fm_queried = 1;
    if (getenv("DOLRECOMP_NO_FASTMEM") || !cpu->external_pointer)
        return;
    u8* base = (u8*)cpu->external_pointer(cpu, 0xFFFFF000u, 0);
    if (!base)
        return;
    g_dolrecomp_fm_cpu = cpu;
    g_dolrecomp_fm_base = base;
    if (!dolrecomp_fastmem_install()) {
        g_dolrecomp_fm_base = NULL;
        fprintf(stderr, "[fastmem] fault handler not installed; staying on the checked path\n");
        return;
    }
    g_dolrecomp_fm_min = 0x06000000u;
    fprintf(stderr, "[fastmem] module guest accesses use the logical view at %p\n", (void*)base);
}
#endif
#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user) {
    g_mem_write_journal = fn;
    g_mem_write_journal_user = user;
}

bool cpu_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));

    cpu->ram_size = GC_MAIN_RAM_SIZE;
    cpu->ram = (u8*)calloc(1, cpu->ram_size);
    if (!cpu->ram) {
        fprintf(stderr, "error: failed to allocate %u bytes for RAM\n", cpu->ram_size);
        return false;
    }

    return true;
}

void cpu_free(CPUState* cpu) {
    if (cpu->ram) {
        free(cpu->ram);
        cpu->ram = NULL;
    }
}

void cpu_reset(CPUState* cpu) {
    u8* ram = cpu->ram;
    u32 ram_size = cpu->ram_size;
    PPCExternalRead external_read = cpu->external_read;
    PPCExternalWrite external_write = cpu->external_write;
    PPCExternalRead32 external_read32 = cpu->external_read32;
    PPCExternalWrite32 external_write32 = cpu->external_write32;
    PPCInstructionFallback instruction_fallback = cpu->instruction_fallback;
    PPCHostCall host_call = cpu->host_call;
    PPCExternalPointer external_pointer = cpu->external_pointer;
    void* external_user_data = cpu->external_user_data;
    u8* exram = cpu->exram;
    u32 exram_size = cpu->exram_size;

    memset(cpu, 0, sizeof(*cpu));
    cpu->ram = ram;
    cpu->ram_size = ram_size;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->external_read32 = external_read32;
    cpu->external_write32 = external_write32;
    cpu->instruction_fallback = instruction_fallback;
    cpu->host_call = host_call;
    cpu->external_pointer = external_pointer;
    cpu->external_user_data = external_user_data;
    cpu->exram = exram;
    cpu->exram_size = exram_size;

    if (cpu->ram)
        memset(cpu->ram, 0, cpu->ram_size);
}

static u32 exception_vector_address(u32 msr, u32 vector) {

    return ((msr & PPC_MSR_IP) ? 0xFFF00000u : 0u) + vector;
}

static u32 exception_msr(u32 old_msr, u32 exception) {
    u32 clear = PPC_MSR_POW | PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_FP |
                PPC_MSR_FE0 | PPC_MSR_SE | PPC_MSR_BE | PPC_MSR_FE1 |
                PPC_MSR_IR | PPC_MSR_DR | PPC_MSR_PM | PPC_MSR_RI |
                PPC_MSR_LE;
    if (exception & PPC_EXC_MACHINE_CHECK)
        clear |= PPC_MSR_ME;

    u32 next = old_msr & ~clear;
    if (old_msr & PPC_MSR_ILE)
        next |= PPC_MSR_LE;
    return next;
}

bool ppc_add_overflowed(u32 a, u32 b, u32 result) {
    return (((a ^ result) & (b ^ result)) >> 31) != 0;
}

void ppc_set_xer_ov(CPUState* cpu, bool ov) {
    cpu->xer = (cpu->xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);
    if (ov)
        cpu->xer |= 0x80000000u;
}

void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info) {
    u32 old_msr = cpu->msr;
    cpu->srr0 = srr0;
    cpu->srr1 = (old_msr & PPC_MSR_RFI_MASK) | srr1_info;
    cpu->exception |= exception;
    cpu->msr = exception_msr(old_msr, exception);
    cpu->pc = exception_vector_address(cpu->msr, vector);
}

void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia) {
    cpu->program_exception |= cause;
    ppc_take_exception(cpu, PPC_EXC_PROGRAM, PPC_VECTOR_PROGRAM, cia, cause);
}

static bool g_ppc_lazy_fp_enabled = true;

void ppc_lazy_fp_set_enabled(bool enabled) {
    g_ppc_lazy_fp_enabled = enabled;
}

bool ppc_fp_available(CPUState* cpu, u32 cia) {
    if (!g_ppc_lazy_fp_enabled || (cpu->msr & PPC_MSR_FP))
        return true;
    ppc_take_exception(cpu, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, cia, 0);
    return false;
}

void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia) {
    if (cpu->instruction_fallback) {
        cpu->instruction_fallback(cpu, raw, cia);
        return;
    }

    (void)raw;
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

bool ppc_host_call(CPUState* cpu, u32 address) {
    return cpu->host_call ? cpu->host_call(cpu, address) : false;
}

void ppc_system_call_exception(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_SYSTEM_CALL, PPC_VECTOR_SYSTEM_CALL, cia + 4u, 0);
}

void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr) {
    cpu->dar = ea;
    cpu->dsisr = dsisr;
    ppc_take_exception(cpu, PPC_EXC_DSI, PPC_VECTOR_DSI, cia, 0);
}

void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia) {
    cpu->dar = ea;
    ppc_take_exception(cpu, PPC_EXC_ALIGNMENT, PPC_VECTOR_ALIGNMENT, cia, 0);
}

u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia) {
    if (tbr == 268)
        return (u32)cpu->timebase;
    if (tbr == 269)
        return (u32)(cpu->timebase >> 32);

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}
static s32 gqr_scale(u32 value) {
    return sign_extend(value & 0x3Fu, 6);
}


static u32 psq_type_size(u8 type) {
    switch (type) {
    case 0: return 4;
    case 4:
    case 6: return 1;
    case 5:
    case 7: return 2;
    default: return 0;
    }
}

/* Quantized load/store semantics mirror Dolphin's interpreter (the chassis
 * lockstep oracle) exactly:
 *  - psq_l/psq_st (non-indexed) require only HID2.LSQE; the indexed forms
 *    are never gated. PSE is not checked, and no alignment exceptions are
 *    raised (unaligned accesses go straight to memory).
 *  - Invalid GQR types 1-3 load 0.0 into both lanes and store nothing.
 *  - Dequantization: f32(int) * f32 power-of-two, rounded once to f32.
 *  - Quantization: round the lane to f32 first, multiply by the f32
 *    power-of-two scale, clamp in f32, truncate. NaN quantizes to 0
 *    (matching SType(NaN-after-clamp) in release Dolphin on arm64). */
static f64 psq_dequant(f64 value, s32 scale) {
    if (scale == 0)
        return (f64)(f32)value;
    return (f64)(f32)ldexp(value, -scale);
}

static f64 psq_load_value(CPUState* cpu, u32 ea, u8 type, s32 scale) {
    switch (type) {
    case 0:
        return f64_value(convert_to_double(mem_read32(cpu, ea)));
    case 4:
        return psq_dequant((f64)mem_read8(cpu, ea), scale);
    case 5:
        return psq_dequant((f64)mem_read16(cpu, ea), scale);
    case 6:
        return psq_dequant((f64)(s8)mem_read8(cpu, ea), scale);
    case 7:
        return psq_dequant((f64)(s16)mem_read16(cpu, ea), scale);
    default:
        return 0.0;
    }
}

static s64 psq_quantize_int(f64 value, s64 min_value, s64 max_value, s32 scale) {
    f32 conv = (f32)value * ldexpf(1.0f, scale);
    if (isnan(conv))
        return 0;
    if (conv <= (f32)min_value)
        return min_value;
    if (conv >= (f32)max_value)
        return max_value;
    return (s64)conv;
}

static void psq_store_value(CPUState* cpu, u32 ea, u8 type, s32 scale, f64 value) {
    switch (type) {
    case 0:
        mem_write32(cpu, ea, convert_to_single_ftz(f64_bits(value)));
        break;
    case 4:
        mem_write8(cpu, ea, (u8)psq_quantize_int(value, 0, 255, scale));
        break;
    case 5:
        mem_write16(cpu, ea, (u16)psq_quantize_int(value, 0, 65535, scale));
        break;
    case 6:
        mem_write8(cpu, ea, (u8)(s8)psq_quantize_int(value, -128, 127, scale));
        break;
    case 7:
        mem_write16(cpu, ea, (u16)(s16)psq_quantize_int(value, -32768, 32767, scale));
        break;
    }
}

static bool psq_check_enabled(CPUState* cpu, bool indexed, u32 cia) {
    if (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    return true;
}

bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 24);
    u8 type = (u8)((gqr >> 16) & 7u);
    u32 size = psq_type_size(type);
    if (size == 0) { /* invalid GQR type: both lanes read as 0.0 */
        cpu->fpr[frD] = 0.0;
        cpu->ps1[frD] = 0.0;
        return true;
    }

    cpu->fpr[frD] = psq_load_value(cpu, ea, type, scale);
    cpu->ps1[frD] = w ? 1.0 : psq_load_value(cpu, ea + size, type, scale);
    return true;
}

bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 8);
    u8 type = (u8)(gqr & 7u);
    u32 size = psq_type_size(type);
    if (size == 0) /* invalid GQR type: nothing is stored */
        return true;

    psq_store_value(cpu, ea, type, scale, cpu->fpr[frS]);
    if (!w)
        psq_store_value(cpu, ea + size, type, scale, cpu->ps1[frS]);
    return true;
}

void ppc_rfi(CPUState* cpu, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->msr = (cpu->msr & ~PPC_MSR_RFI_MASK) | (cpu->srr1 & PPC_MSR_RFI_MASK);
    cpu->msr &= ~PPC_MSR_POW;
    cpu->pc = cpu->srr0 & ~3u;
}

void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    if ((cpu->hid2 & PPC_HID2_LCE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return;
    }

    u32 block = ea & ~31u;
    u32 slot = (block >> 5) & 511u;
    bool hit = cpu->locked_cache_valid[slot] && cpu->locked_cache_tag[slot] == block;
    bool first_hit_error = hit && (cpu->hid2 & PPC_HID2_DCHERR) == 0;

    if (hit) {
        cpu->hid2 |= PPC_HID2_DCHERR;
        if (first_hit_error && (cpu->hid2 & PPC_HID2_DCHEE) &&
            (cpu->msr & PPC_MSR_EE) && (cpu->msr & PPC_MSR_ME)) {
            ppc_take_exception(cpu, PPC_EXC_MACHINE_CHECK, PPC_VECTOR_MACHINE_CHECK,
                               cia, PPC_SRR1_MACHINE_CHECK_DCBZL);
        }
    } else {
        cpu->locked_cache_valid[slot] = true;
        cpu->locked_cache_tag[slot] = block;
    }

    for (u32 i = 0; i < 32; i += 4)
        mem_write32(cpu, block + i, 0);
}

u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return 0;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return 0;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_rid = rid;
    cpu->external_read_count++;
    if (cpu->external_read32)
        return cpu->external_read32(cpu, ea, rid);
    return 0;
}

void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
    cpu->external_write_count++;
    if (cpu->external_write32)
        cpu->external_write32(cpu, ea, value, rid);
}

void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->tlb_last_vps = (ea >> 12) & 0xFFFFu;
    cpu->tlb_last_index = (ea >> 12) & 0x3Fu;
    cpu->tlb_invalidate_count++;
}

bool ppc_trap_condition(u8 to, u32 a, u32 b) {
    s32 sa = (s32)a;
    s32 sb = (s32)b;

    return ((sa < sb) && (to & 0x10u)) ||
           ((sa > sb) && (to & 0x08u)) ||
           ((sa == sb) && (to & 0x04u)) ||
           ((a < b) && (to & 0x02u)) ||
           ((a > b) && (to & 0x01u));
}
