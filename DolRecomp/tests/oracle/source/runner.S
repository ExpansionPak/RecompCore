    .text
    .globl oracle_enter
    .globl oracle_capture
    .globl oracle_exception_recover
    .globl oracle_fpu_exception_handler
    .globl oracle_fpu_vector_stub
    .globl oracle_fpu_vector_stub_end

/*
 * void oracle_enter(const OracleInput* in, OracleOutput* out, void* code)
 *
 * The generated code buffer starts by restoring the requested guest CTR, then
 * executes the raw probe words, then branches to oracle_capture.  The runner
 * therefore may use CTR to enter the buffer without corrupting the test's
 * architectural CTR.
 */
oracle_enter:
    stwu    1,-288(1)
    stw     2,8(1)
    stw     13,12(1)
    mflr    0
    stw     0,16(1)
    stw     14,20(1)
    stw     15,24(1)
    stw     16,28(1)
    stw     17,32(1)
    stw     18,36(1)
    stw     19,40(1)
    stw     20,44(1)
    stw     21,48(1)
    stw     22,52(1)
    stw     23,56(1)
    stw     24,60(1)
    stw     25,64(1)
    stw     26,68(1)
    stw     27,72(1)
    stw     28,76(1)
    stw     29,80(1)
    stw     30,84(1)
    stw     31,88(1)

    mfmsr   0
    stw     0,252(1)
    lis     6,oracle_host_msr@ha
    stw     0,oracle_host_msr@l(6)
    ori     0,0,0x2000
    mtmsr   0
    isync

    stfd    14,96(1)
    stfd    15,104(1)
    stfd    16,112(1)
    stfd    17,120(1)
    stfd    18,128(1)
    stfd    19,136(1)
    stfd    20,144(1)
    stfd    21,152(1)
    stfd    22,160(1)
    stfd    23,168(1)
    stfd    24,176(1)
    stfd    25,184(1)
    stfd    26,192(1)
    stfd    27,200(1)
    stfd    28,208(1)
    stfd    29,216(1)
    stfd    30,224(1)
    stfd    31,232(1)

    mfcr    0
    stw     0,240(1)
    mfxer   0
    stw     0,244(1)
    mfctr   0
    stw     0,248(1)
    mffs    0
    stfd    0,256(1)
    mfspr   0,26
    stw     0,264(1)
    mfspr   0,27
    stw     0,268(1)
    mfspr   0,19
    stw     0,272(1)
    mfspr   0,18
    stw     0,276(1)
    mfspr   0,282
    stw     0,280(1)
    lis     6,oracle_host_frame@ha
    stw     1,oracle_host_frame@l(6)
    mfsprg  0,2
    lis     6,oracle_host_sprg2@ha
    stw     0,oracle_host_sprg2@l(6)
    mfsprg  0,3
    lis     6,oracle_host_sprg3@ha
    stw     0,oracle_host_sprg3@l(6)

    /* The exception handler may observe a different cache context.  Publish
       the complete host save frame and its global anchors before guest code. */
    mr      6,1
    li      7,10
    mtctr   7
.Lflush_host_frame:
    dcbf    0,6
    addi    6,6,32
    bdnz    .Lflush_host_frame
    lis     6,oracle_host_frame@ha
    addi    6,6,oracle_host_frame@l
    dcbf    0,6
    sync

    /* Load both 64-bit halves of every paired-single register. f0 is the
       temporary, then is restored with PS1=PS0 as documented by OracleInput. */
    .macro load_pair reg, ps0off, ps1off
    lfd     \reg,\ps0off(3)
    lfd     0,\ps1off(3)
    ps_merge00 \reg,\reg,0
    .endm
    load_pair 1,136,392
    load_pair 2,144,400
    load_pair 3,152,408
    load_pair 4,160,416
    load_pair 5,168,424
    load_pair 6,176,432
    load_pair 7,184,440
    load_pair 8,192,448
    load_pair 9,200,456
    load_pair 10,208,464
    load_pair 11,216,472
    load_pair 12,224,480
    load_pair 13,232,488
    load_pair 14,240,496
    load_pair 15,248,504
    load_pair 16,256,512
    load_pair 17,264,520
    load_pair 18,272,528
    load_pair 19,280,536
    load_pair 20,288,544
    load_pair 21,296,552
    load_pair 22,304,560
    load_pair 23,312,568
    load_pair 24,320,576
    load_pair 25,328,584
    load_pair 26,336,592
    load_pair 27,344,600
    load_pair 28,352,608
    load_pair 29,360,616
    load_pair 30,368,624
    load_pair 31,376,632
    lfd     0,664(3)
    mtfsf   0xff,0
    lfd     0,128(3)
    ps_merge00 0,0,0

    lwz     0,640(3)
    mtcrf   0xff,0
    lwz     0,644(3)
    mtxer   0
    lwz     0,648(3)
    mtlr    0
    lwz     0,652(3)
    mtctr   0
    lwz     0,656(3)
    mtmsr   0
    isync
    lwz     0,672(3)
    mtspr   26,0
    lwz     0,676(3)
    mtspr   27,0
    li      0,0
    mtspr   19,0
    mtspr   18,0
    mtspr   282,0
    lwz     0,0(3)
    lwz     1,4(3)
    lwz     2,8(3)
    lwz     4,16(3)
    lwz     5,20(3)
    lwz     6,24(3)
    lwz     7,28(3)
    lwz     8,32(3)
    lwz     9,36(3)
    lwz     10,40(3)
    lwz     11,44(3)
    lwz     12,48(3)
    lwz     13,52(3)
    lwz     14,56(3)
    lwz     15,60(3)
    lwz     16,64(3)
    lwz     17,68(3)
    lwz     18,72(3)
    lwz     19,76(3)
    lwz     20,80(3)
    lwz     21,84(3)
    lwz     22,88(3)
    lwz     23,92(3)
    lwz     24,96(3)
    lwz     25,100(3)
    lwz     26,104(3)
    lwz     27,108(3)
    lwz     28,112(3)
    lwz     29,116(3)
    lwz     30,120(3)
    lwz     31,124(3)
    lwz     3,12(3)

    b       code_buf

oracle_capture:
    /* r11 is the output pointer.  The generated postlude saved guest r11
       in SPRG1 and restored guest r0 before branching here. */
    stw     0,8(11)
    stw     1,12(11)
    stw     2,16(11)
    stw     3,20(11)
    stw     4,24(11)
    stw     5,28(11)
    stw     6,32(11)
    stw     7,36(11)
    stw     8,40(11)
    stw     9,44(11)
    stw     10,48(11)
    mfsprg  0,1
    stw     0,52(11)
    stw     12,56(11)
    stw     13,60(11)
    stw     14,64(11)
    stw     15,68(11)
    stw     16,72(11)
    stw     17,76(11)
    stw     18,80(11)
    stw     19,84(11)
    stw     20,88(11)
    stw     21,92(11)
    stw     22,96(11)
    stw     23,100(11)
    stw     24,104(11)
    stw     25,108(11)
    stw     26,112(11)
    stw     27,116(11)
    stw     28,120(11)
    stw     29,124(11)
    stw     30,128(11)
    stw     31,132(11)

    stfd    0,136(11)
    stfd    1,144(11)
    stfd    2,152(11)
    stfd    3,160(11)
    stfd    4,168(11)
    stfd    5,176(11)
    stfd    6,184(11)
    stfd    7,192(11)
    stfd    8,200(11)
    stfd    9,208(11)
    stfd    10,216(11)
    stfd    11,224(11)
    stfd    12,232(11)
    stfd    13,240(11)
    stfd    14,248(11)
    stfd    15,256(11)
    stfd    16,264(11)
    stfd    17,272(11)
    stfd    18,280(11)
    stfd    19,288(11)
    stfd    20,296(11)
    stfd    21,304(11)
    stfd    22,312(11)
    stfd    23,320(11)
    stfd    24,328(11)
    stfd    25,336(11)
    stfd    26,344(11)
    stfd    27,352(11)
    stfd    28,360(11)
    stfd    29,368(11)
    stfd    30,376(11)
    stfd    31,384(11)

    /* Capture PS1 without narrowing it to f32. PS0 was already saved above,
       so f0 is now available as a destructive extraction temporary. */
    .macro capture_ps1 reg, off
    ps_merge11 0,\reg,\reg
    stfd    0,\off(11)
    .endm
    capture_ps1 0,392
    capture_ps1 1,400
    capture_ps1 2,408
    capture_ps1 3,416
    capture_ps1 4,424
    capture_ps1 5,432
    capture_ps1 6,440
    capture_ps1 7,448
    capture_ps1 8,456
    capture_ps1 9,464
    capture_ps1 10,472
    capture_ps1 11,480
    capture_ps1 12,488
    capture_ps1 13,496
    capture_ps1 14,504
    capture_ps1 15,512
    capture_ps1 16,520
    capture_ps1 17,528
    capture_ps1 18,536
    capture_ps1 19,544
    capture_ps1 20,552
    capture_ps1 21,560
    capture_ps1 22,568
    capture_ps1 23,576
    capture_ps1 24,584
    capture_ps1 25,592
    capture_ps1 26,600
    capture_ps1 27,608
    capture_ps1 28,616
    capture_ps1 29,624
    capture_ps1 30,632
    capture_ps1 31,640

    mfcr    0
    stw     0,648(11)
    mfxer   0
    stw     0,652(11)
    mflr    0
    stw     0,656(11)
    mfmsr   0
    stw     0,664(11)
    mffs    0
    stfd    0,672(11)

    mfspr   0,26
    stw     0,940(11)
    mfspr   0,27
    stw     0,944(11)
    mfspr   0,19
    stw     0,948(11)
    mfspr   0,18
    stw     0,952(11)
    mfspr   0,282
    stw     0,956(11)
    mfspr   0,920
    stw     0,960(11)
    .macro capture_sr index, off
    mfsr    0,\index
    stw     0,\off(11)
    .endm
    capture_sr 0,964
    capture_sr 1,968
    capture_sr 2,972
    capture_sr 3,976
    capture_sr 4,980
    capture_sr 5,984
    capture_sr 6,988
    capture_sr 7,992
    capture_sr 8,996
    capture_sr 9,1000
    capture_sr 10,1004
    capture_sr 11,1008
    capture_sr 12,1012
    capture_sr 13,1016
    capture_sr 14,1020
    capture_sr 15,1024
    .macro capture_gqr number, off
    mfspr   0,\number
    stw     0,\off(11)
    .endm
    capture_gqr 912,1028
    capture_gqr 913,1032
    capture_gqr 914,1036
    capture_gqr 915,1040
    capture_gqr 916,1044
    capture_gqr 917,1048
    capture_gqr 918,1052
    capture_gqr 919,1056

    b       .Lrestore_host

oracle_exception_recover:
    /* libogc's exception epilogue reaches here through rfi after the panic
       callback redirects the saved context. */

.Lrestore_host:
    lis     1,oracle_host_frame@ha
    lwz     1,oracle_host_frame@l(1)
    lwz     2,8(1)
    lwz     13,12(1)
    lwz     0,16(1)
    mtlr    0
    lwz     14,20(1)
    lwz     15,24(1)
    lwz     16,28(1)
    lwz     17,32(1)
    lwz     18,36(1)
    lwz     19,40(1)
    lwz     20,44(1)
    lwz     21,48(1)
    lwz     22,52(1)
    lwz     23,56(1)
    lwz     24,60(1)
    lwz     25,64(1)
    lwz     26,68(1)
    lwz     27,72(1)
    lwz     28,76(1)
    lwz     29,80(1)
    lwz     30,84(1)
    lwz     31,88(1)
    lfd     14,96(1)
    lfd     15,104(1)
    lfd     16,112(1)
    lfd     17,120(1)
    lfd     18,128(1)
    lfd     19,136(1)
    lfd     20,144(1)
    lfd     21,152(1)
    lfd     22,160(1)
    lfd     23,168(1)
    lfd     24,176(1)
    lfd     25,184(1)
    lfd     26,192(1)
    lfd     27,200(1)
    lfd     28,208(1)
    lfd     29,216(1)
    lfd     30,224(1)
    lfd     31,232(1)
    lfd     0,256(1)
    mtfsf   0xff,0
    lwz     0,264(1)
    mtspr   26,0
    lwz     0,268(1)
    mtspr   27,0
    lwz     0,272(1)
    mtspr   19,0
    lwz     0,276(1)
    mtspr   18,0
    lwz     0,280(1)
    mtspr   282,0
    lwz     0,244(1)
    mtxer   0
    lwz     0,248(1)
    mtctr   0
    lwz     0,240(1)
    mtcrf   0xff,0
    lis     12,oracle_host_msr@ha
    lwz     0,oracle_host_msr@l(12)
    mtmsr   0
    isync
    addi    1,1,288
    blr

oracle_fpu_exception_handler:
    /* The vector stub preserved faulting r0/r11/r12 in SPRG0/1/2 and SRR0
       in SPRG3, then returned here with translation and FP enabled. */
    lis     11,active_output@ha
    lwz     11,active_output@l(11)

    mfsprg  0,0
    stw     0,8(11)
    stw     1,12(11)
    stw     2,16(11)
    stw     3,20(11)
    stw     4,24(11)
    stw     5,28(11)
    stw     6,32(11)
    stw     7,36(11)
    stw     8,40(11)
    stw     9,44(11)
    stw     10,48(11)
    mfsprg  0,1
    stw     0,52(11)
    mfsprg  0,2
    stw     0,56(11)
    stw     13,60(11)
    stw     14,64(11)
    stw     15,68(11)
    stw     16,72(11)
    stw     17,76(11)
    stw     18,80(11)
    stw     19,84(11)
    stw     20,88(11)
    stw     21,92(11)
    stw     22,96(11)
    stw     23,100(11)
    stw     24,104(11)
    stw     25,108(11)
    stw     26,112(11)
    stw     27,116(11)
    stw     28,120(11)
    stw     29,124(11)
    stw     30,128(11)
    stw     31,132(11)

    li      0,1
    stw     0,0(11)
    li      0,8
    stw     0,4(11)

    mfsprg  0,3
    stw     0,936(11)
    mfmsr   0
    xori    0,0,0x2000
    stw     0,664(11)
    mfcr    0
    stw     0,648(11)
    mfxer   0
    stw     0,652(11)
    mflr    0
    stw     0,656(11)
    mfctr   0
    stw     0,660(11)

    lis     12,oracle_host_sprg2@ha
    lwz     0,oracle_host_sprg2@l(12)
    mtsprg  2,0
    lis     12,oracle_host_sprg3@ha
    lwz     0,oracle_host_sprg3@l(12)
    mtsprg  3,0

    lis     12,oracle_host_msr@ha
    lwz     0,oracle_host_msr@l(12)
    ori     0,0,0x2000
    mtmsr   0
    isync
    b       oracle_exception_recover

oracle_fpu_vector_stub:
    mtsprg  0,0
    mtsprg  1,11
    mtsprg  2,12
    mfsrr0  11
    mtsprg  3,11
    mfsrr1  11
    ori     11,11,0x2030
    mtsrr1  11
    lis     11,oracle_fpu_exception_handler@ha
    addi    11,11,oracle_fpu_exception_handler@l
    mtsrr0  11
    rfi
oracle_fpu_vector_stub_end:
