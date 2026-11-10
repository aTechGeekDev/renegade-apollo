; apollowifi_asm.asm - the few ARM64 instructions apollowifi.c must issue exactly (armasm64; as apollogpu_asm.asm).
;
; Register accesses are one plain ldr / str each: the hypervisor emulates the GPU SMMU's global registers and can
; only carry out single-register loads and stores without writeback, and a store of the zero register "goes horribly
; wrong" (HackiOS redwood_adreno.c; ios-apollo sweet_gpu.c gpu_rd/gpu_wr). The value to store is always in w1 here,
; never wzr. Barriers as ios-apollo: dmb oshld after a read, dmb oshst / dsb osh around a write.

        AREA    |.text|, CODE, READONLY, ALIGN=3

        EXPORT  apollo_rd32
        EXPORT  apollo_wr32
        EXPORT  apollo_dc_civac
        EXPORT  apollo_dsb_sy
        EXPORT  apollo_smc

; ULONG apollo_rd32(ULONG_PTR address)
apollo_rd32 PROC
        ldr     w0, [x0]
        dmb     oshld
        ret
        ENDP

; VOID apollo_wr32(ULONG_PTR address, ULONG value)
apollo_wr32 PROC
        dmb     oshst
        str     w1, [x0]
        dsb     osh
        ret
        ENDP

; VOID apollo_dc_civac(ULONG_PTR start, SIZE_T size): clean and invalidate [start, start+size) by 64-byte line, then dsb sy
apollo_dc_civac PROC
        add     x1, x0, x1
        bic     x0, x0, #63
civac_loop
        cmp     x0, x1
        b.hs    civac_done
        dc      civac, x0
        add     x0, x0, #64
        b       civac_loop
civac_done
        dsb     sy
        ret
        ENDP

; VOID apollo_dsb_sy(VOID)
apollo_dsb_sy PROC
        dsb     sy
        ret
        ENDP

; VOID apollo_smc(UINT64 regs[8]): x0..x7 from regs, smc #0, x0..x7 back into regs (SMC calling convention; x19 and
; x30 are kept on the stack, x8..x17 may be lost, as qcom_scm-smc.c / ios-apollo gpu_scm assume)
apollo_smc PROC
        stp     x19, x30, [sp, #-16]!
        mov     x19, x0
        ldp     x0, x1, [x19]
        ldp     x2, x3, [x19, #16]
        ldp     x4, x5, [x19, #32]
        ldp     x6, x7, [x19, #48]
        smc     #0
        stp     x0, x1, [x19]
        stp     x2, x3, [x19, #16]
        stp     x4, x5, [x19, #32]
        stp     x6, x7, [x19, #48]
        ldp     x19, x30, [sp], #16
        ret
        ENDP

        END
