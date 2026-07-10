#include <gccore.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuxedo/ppc/exception.h>
#include <tuxedo/ppc/intrinsics.h>

extern void oracle_enter(const void* in, void* out, void* code);
extern void oracle_capture(void);
extern void oracle_exception_recover(void);
extern void oracle_fpu_exception_handler(void);
extern const u32 oracle_fpu_vector_stub[];
extern const u32 oracle_fpu_vector_stub_end[];
#define ORACLE_MEM_SIZE 256u
#define ORACLE_MAX_WORDS 16u
#define ORACLE_CAPTURE_TEXT_SIZE (2u * 1024u * 1024u)
#define MSR_FP_BIT 0x00002000u

typedef struct {
    u32 gpr[32];
    u64 fpr[32];
    u64 ps1[32];
    u32 cr;
    u32 xer;
    u32 lr;
    u32 ctr;
    u32 msr;
    u64 fpscr;
    u32 srr0;
    u32 srr1;
    u32 hid2;
    u32 sr[16];
    u32 gqr[8];
} OracleInput;

typedef struct {
    u32 exception;
    u32 exid;
    u32 gpr[32];
    u64 fpr[32];
    u64 ps[32];
    u32 cr;
    u32 xer;
    u32 lr;
    u32 ctr;
    u32 msr;
    u64 fpscr;
    u8 mem[ORACLE_MEM_SIZE];
    u32 pc;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u32 sr[16];
    u32 gqr[8];
} OracleOutput;

typedef struct {
    const char* name;
    const u32* words;
    u32 word_count;
    OracleInput in;
    u8 mem[ORACLE_MEM_SIZE];
    u32 mem_len;
    u32 guest_mem_base;
    u32 trap_fpu;
} OracleCase;

static u8 oracle_mem[ORACLE_MEM_SIZE] ATTRIBUTE_ALIGN(32);
u32 code_buf[ORACLE_MAX_WORDS + 12] ATTRIBUTE_ALIGN(32);
u32 oracle_host_frame;
u32 oracle_host_msr;
u32 oracle_host_sprg2;
u32 oracle_host_sprg3;
OracleOutput* active_output ATTRIBUTE_ALIGN(32);
static const OracleInput* active_input;
static u32 active_capture_pc;
static char capture_text[ORACLE_CAPTURE_TEXT_SIZE] ATTRIBUTE_ALIGN(32);
static size_t capture_text_len ATTRIBUTE_ALIGN(32);
static int capture_text_overflow ATTRIBUTE_ALIGN(32);
static volatile u32 capture_ready ATTRIBUTE_ALIGN(32);
/* Nonzero initializer keeps this in .data so a pre-entry GDB write is not
   overwritten later by libogc's BSS clear. The driver always writes it. */
volatile u32 oracle_case_start ATTRIBUTE_ALIGN(32) = 0xFFFFFFFFu;
volatile u32 oracle_case_end ATTRIBUTE_ALIGN(32) = 0xFFFFFFFFu;
static size_t capture_build_len;
static int capture_build_overflow;
static u32 saved_fpu_vector[64] ATTRIBUTE_ALIGN(32);

_Static_assert(offsetof(OracleInput, fpr) == 128, "OracleInput.fpr offset");
_Static_assert(offsetof(OracleInput, ps1) == 384, "OracleInput.ps1 offset");
_Static_assert(offsetof(OracleInput, cr) == 640, "OracleInput.cr offset");
_Static_assert(offsetof(OracleInput, msr) == 656, "OracleInput.msr offset");
_Static_assert(offsetof(OracleInput, fpscr) == 664, "OracleInput.fpscr offset");
_Static_assert(offsetof(OracleInput, srr0) == 672, "OracleInput.srr0 offset");
_Static_assert(offsetof(OracleInput, sr) == 684, "OracleInput.sr offset");
_Static_assert(offsetof(OracleInput, gqr) == 748, "OracleInput.gqr offset");
_Static_assert(sizeof(OracleInput) == 784, "OracleInput size");
_Static_assert(offsetof(OracleOutput, gpr) == 8, "OracleOutput.gpr offset");
_Static_assert(offsetof(OracleOutput, fpr) == 136, "OracleOutput.fpr offset");
_Static_assert(offsetof(OracleOutput, ps) == 392, "OracleOutput.ps offset");
_Static_assert(offsetof(OracleOutput, cr) == 648, "OracleOutput.cr offset");
_Static_assert(offsetof(OracleOutput, fpscr) == 672, "OracleOutput.fpscr offset");
_Static_assert(offsetof(OracleOutput, mem) == 680, "OracleOutput.mem offset");
_Static_assert(offsetof(OracleOutput, pc) == 936, "OracleOutput.pc offset");
_Static_assert(offsetof(OracleOutput, srr0) == 940, "OracleOutput.srr0 offset");
_Static_assert(offsetof(OracleOutput, sr) == 964, "OracleOutput.sr offset");
_Static_assert(offsetof(OracleOutput, gqr) == 1028, "OracleOutput.gqr offset");
_Static_assert(sizeof(OracleOutput) == 1064, "OracleOutput size");

static const u32 seq_msr_fp_clear[] = {0xEC22182Au};

#include "../generated_cases.inc"

static u32 mfmsr_local(void) {
    u32 v;
    asm volatile("mfmsr %0" : "=r"(v));
    return v;
}

static void read_special_input(OracleInput* in) {
#define READ_SR(index) asm volatile("mfsr %0," #index : "=r"(in->sr[index]))
#define READ_SPR(field, number) \
    asm volatile("mfspr %0," #number : "=r"(in->field))
#define READ_GQR(index, number) \
    asm volatile("mfspr %0," #number : "=r"(in->gqr[index]))
    READ_SPR(srr0, 26);
    READ_SPR(srr1, 27);
    READ_SPR(hid2, 920);
    READ_SR(0); READ_SR(1); READ_SR(2); READ_SR(3);
    READ_SR(4); READ_SR(5); READ_SR(6); READ_SR(7);
    READ_SR(8); READ_SR(9); READ_SR(10); READ_SR(11);
    READ_SR(12); READ_SR(13); READ_SR(14); READ_SR(15);
    READ_GQR(0, 912); READ_GQR(1, 913); READ_GQR(2, 914); READ_GQR(3, 915);
    READ_GQR(4, 916); READ_GQR(5, 917); READ_GQR(6, 918); READ_GQR(7, 919);
#undef READ_GQR
#undef READ_SPR
#undef READ_SR
}

static u64 double_bits(double value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void capture_append(const char* fmt, ...) {
    int written;
    va_list args;

    if (capture_build_overflow)
        return;

    va_start(args, fmt);
    written = vsnprintf(capture_text + capture_build_len,
                        sizeof(capture_text) - capture_build_len, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= sizeof(capture_text) - capture_build_len) {
        capture_build_overflow = 1;
        return;
    }
    capture_build_len += (size_t)written;
}

__attribute__((noinline)) void oracle_done(void) {
    asm volatile("" ::: "memory");
}

static void copy_context_output(OracleOutput* out, unsigned exid, const PPCContext* ctx) {
    memset(out, 0, sizeof(*out));
    out->exception = 1;
    out->exid = exid;
    out->pc = ctx->pc;
    out->cr = ctx->cr;
    out->xer = ctx->xer;
    out->lr = ctx->lr;
    out->ctr = ctx->ctr;
    out->msr = ctx->msr;
    for (unsigned i = 0; i < 32; i++) {
        out->gpr[i] = ctx->gpr[i];
        out->fpr[i] = active_input->fpr[i];
        out->ps[i] = active_input->ps1[i];
    }
    out->fpscr = active_input->fpscr;
    out->srr0 = ctx->pc;
    out->srr1 = ctx->msr;
    out->hid2 = active_input->hid2;
    memcpy(out->sr, active_input->sr, sizeof(out->sr));
    memcpy(out->gqr, active_input->gqr, sizeof(out->gqr));
    memcpy(out->mem, oracle_mem, sizeof(out->mem));
}

static void oracle_panic(unsigned exid, PPCContext* ctx) {
    if (active_output) {
        copy_context_output(active_output, exid, ctx);
        ctx->pc = (u32)(uintptr_t)oracle_exception_recover;
        ctx->msr = oracle_host_msr;
    }
}

static void install_fpu_probe_handler(void) {
    u32* target = (u32*)0x80000800u;
    u32 size = (u32)((uintptr_t)oracle_fpu_vector_stub_end -
                     (uintptr_t)oracle_fpu_vector_stub);
    memcpy(saved_fpu_vector, target, sizeof(saved_fpu_vector));
    memcpy(target, oracle_fpu_vector_stub, size);
    DCFlushRange(target, 0x100);
    ICInvalidateRange(target, 0x100);
}

static void restore_fpu_handler(void) {
    u32* target = (u32*)0x80000800u;
    memcpy(target, saved_fpu_vector, sizeof(saved_fpu_vector));
    DCFlushRange(target, sizeof(saved_fpu_vector));
    ICInvalidateRange(target, sizeof(saved_fpu_vector));
}

static void build_code(const OracleCase* c, OracleOutput* out) {
    u32 n = 0;
    for (u32 i = 0; i < c->word_count; i++)
        code_buf[n++] = c->words[i];
    active_capture_pc = (u32)(uintptr_t)&code_buf[n];
    code_buf[n++] = 0x7C1043A6u;        /* mtsprg 0,r0 */
    code_buf[n++] = 0x7D7143A6u;        /* mtsprg 1,r11 */
    code_buf[n++] = 0x7C0902A6u;        /* mfctr r0 */
    code_buf[n++] = 0x3D600000u | ((u32)(uintptr_t)out >> 16);
    code_buf[n++] = 0x616B0000u | ((u32)(uintptr_t)out & 0xFFFFu);
    code_buf[n++] = 0x900B0294u;        /* stw r0,660(r11) */
    code_buf[n++] = 0x3C000000u | ((u32)(uintptr_t)oracle_capture >> 16);
    code_buf[n++] = 0x60000000u | ((u32)(uintptr_t)oracle_capture & 0xFFFFu);
    code_buf[n++] = 0x7C0903A6u;        /* mtctr r0 */
    code_buf[n++] = 0x7C1042A6u;        /* mfsprg r0,0 */
    code_buf[n++] = 0x4E800420u;        /* bctr */
    DCFlushRange(code_buf, n * sizeof(code_buf[0]));
    ICInvalidateRange(code_buf, n * sizeof(code_buf[0]));
}

static void emit_hex_words(const char* prefix, const u32* values, unsigned count) {
    capture_append("%s", prefix);
    for (unsigned i = 0; i < count; i++)
        capture_append("%s%08X", i ? "," : "", values[i]);
    capture_append("\n");
}

static void emit_hex64_words(const char* prefix, const u64* values, unsigned count) {
    capture_append("%s", prefix);
    for (unsigned i = 0; i < count; i++)
        capture_append("%s%08X%08X", i ? "," : "",
                       (u32)(values[i] >> 32), (u32)values[i]);
    capture_append("\n");
}

static void emit_mem(const OracleOutput* out, u32 len) {
    capture_append("MEM,");
    for (u32 i = 0; i < len; i++)
        capture_append("%02X", out->mem[i]);
    capture_append("\n");
}

static void run_case(OracleCase* c, OracleOutput* out) {
    int trap_active = c->trap_fpu && !(c->in.msr & MSR_FP_BIT);

    memset(out, 0, sizeof(*out));
    memset(oracle_mem, 0, sizeof(oracle_mem));
    memcpy(oracle_mem, c->mem, c->mem_len);

    if (c->guest_mem_base) {
        for (unsigned i = 0; i < 32; i++) {
            u32 value = c->in.gpr[i];
            if (value >= c->guest_mem_base &&
                value - c->guest_mem_base < ORACLE_MEM_SIZE) {
                c->in.gpr[i] =
                    (u32)(uintptr_t)(oracle_mem + value - c->guest_mem_base);
            }
        }
    }
    /* f0 is the loader's temporary. Its two lanes are intentionally seeded
       from the same PS0 value; all other registers preserve independent PS1. */
    c->in.ps1[0] = c->in.fpr[0];
    build_code(c, out);

    if (trap_active)
        install_fpu_probe_handler();
    active_output = out;
    active_input = &c->in;
    DCFlushRange(&active_output, 32);
    oracle_enter(&c->in, out, code_buf);
    if (out->exception) {
        for (unsigned i = 0; i < 32; i++) {
            out->fpr[i] = c->in.fpr[i];
            out->ps[i] = c->in.ps1[i];
        }
        out->fpscr = c->in.fpscr;
    }
    if (!out->exception)
        out->pc = active_capture_pc;
    active_output = NULL;
    active_input = NULL;
    active_capture_pc = 0;
    if (trap_active)
        restore_fpu_handler();

    memcpy(out->mem, oracle_mem, sizeof(out->mem));
}

static void init_case_defaults(OracleCase* c, u32 msr) {
    c->in.msr = msr;
    c->in.lr = 0x817F0000u;
    c->in.ctr = 0x817F1000u;
    read_special_input(&c->in);
    c->in.srr0 = 0x817F2000u;
    c->in.srr1 = msr;
}

static void emit_case(const OracleCase* c, const OracleOutput* out) {
    capture_append("CASE,%s,%u,exception,%u,exid,%u,pc,%08X,cr,%08X,xer,%08X,lr,%08X,ctr,%08X,msr,%08X,fpscr,%08X%08X,guest_mem_base,%08X,oracle_mem_base,%08X\n",
                   c->name, c->word_count, out->exception, out->exid,
                   out->pc, out->cr, out->xer, out->lr, out->ctr, out->msr,
                   (u32)(out->fpscr >> 32), (u32)out->fpscr,
                   c->guest_mem_base, (u32)(uintptr_t)oracle_mem);
    emit_hex_words("RAW,", c->words, c->word_count);
    capture_append("INPUT,cr,%08X,xer,%08X,lr,%08X,ctr,%08X,msr,%08X,fpscr,%08X%08X\n",
                   c->in.cr, c->in.xer, c->in.lr, c->in.ctr, c->in.msr,
                   (u32)(c->in.fpscr >> 32), (u32)c->in.fpscr);
    emit_hex_words("IN_GPR,", c->in.gpr, 32);
    emit_hex64_words("IN_FPR,", c->in.fpr, 32);
    emit_hex64_words("IN_PS1,", c->in.ps1, 32);
    capture_append("IN_SPECIAL,srr0,%08X,srr1,%08X,hid2,%08X\n",
                   c->in.srr0, c->in.srr1, c->in.hid2);
    emit_hex_words("IN_SR,", c->in.sr, 16);
    emit_hex_words("IN_GQR,", c->in.gqr, 8);
    capture_append("MEM_IN,");
    for (u32 i = 0; i < ORACLE_MEM_SIZE; i++)
        capture_append("%02X", c->mem[i]);
    capture_append("\n");
    emit_hex_words("GPR,", out->gpr, 32);
    emit_hex64_words("FPR,", out->fpr, 32);
    emit_hex64_words("PS1,", out->ps, 32);
    capture_append("SPECIAL,srr0,%08X,srr1,%08X,dar,%08X,dsisr,%08X,ear,%08X,hid2,%08X\n",
                   out->srr0, out->srr1, out->dar, out->dsisr, out->ear,
                   out->hid2);
    emit_hex_words("SR,", out->sr, 16);
    emit_hex_words("GQR,", out->gqr, 8);
    emit_mem(out, ORACLE_MEM_SIZE);
    capture_append("ENDCASE,%s\n", c->name);
}

int main(void) {
    VIDEO_Init();
    void* xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(VIDEO_GetPreferredMode(NULL)));
    GXRModeObj* rmode = VIDEO_GetPreferredMode(NULL);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    PPCExcptCurPanicFn = oracle_panic;

    DCFlushRange(capture_text, sizeof(capture_text));
    DCFlushRange(&capture_text_len, 32);
    DCFlushRange(&capture_text_overflow, 32);
    DCFlushRange((void*)&capture_ready, 32);

    u32 base_msr = mfmsr_local() | MSR_FP_BIT;
    OracleCase fp_exception = {
        .name = "msr_fp_clear",
        .words = seq_msr_fp_clear,
        .word_count = 1,
        .in = {.fpr = {[2] = double_bits(1.25), [3] = double_bits(2.5)}},
        .trap_fpu = 1,
    };
    init_case_defaults(&fp_exception, base_msr);
    fp_exception.in.msr &= ~MSR_FP_BIT;

    u32 first_case = oracle_case_start;
    u32 end_case = oracle_case_end;
    if (first_case > generated_case_count)
        first_case = generated_case_count;
    if (end_case > generated_case_count)
        end_case = generated_case_count;
    if (end_case < first_case)
        end_case = first_case;
    u32 selected_count = end_case - first_case;
    int include_exception = end_case == generated_case_count;

    capture_append("ORACLE_BEGIN,version,2,cases,%u,mem_size,%u\n",
                   selected_count + (unsigned)include_exception, ORACLE_MEM_SIZE);

    OracleOutput out;
    for (unsigned i = first_case; i < end_case; i++) {
        OracleCase c = generated_cases[i];
        init_case_defaults(&c, base_msr);
        run_case(&c, &out);
        emit_case(&c, &out);
    }
    if (include_exception) {
        run_case(&fp_exception, &out);
        emit_case(&fp_exception, &out);
    }

    capture_append("ORACLE_END\n");
    capture_text_len = capture_build_len;
    capture_text_overflow = capture_build_overflow;
    DCFlushRange(capture_text, sizeof(capture_text));
    DCFlushRange(&capture_text_len, 32);
    DCFlushRange(&capture_text_overflow, 32);
    capture_ready = 0x4F52434Cu;
    DCFlushRange((void*)&capture_ready, 32);
    oracle_done();
    while (1)
        VIDEO_WaitVSync();
    return 0;
}
