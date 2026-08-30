.text
.align 2

.hidden swipegate_hook_enter_and_gate
.hidden swipegate_hook_exit
.hidden gSwipeGateOriginalOnSwipeProcess
.global swipegate_on_swipe_process_hook
.hidden swipegate_on_swipe_process_hook
.type swipegate_on_swipe_process_hook, %function

// ABI-transparent wrapper for GestureInputBackHelper::on_swipe_process.
// The only intentionally changed incoming register is s0 (horizontal distance).
// Integer arguments x0-x8 and floating/vector arguments q0-q7 are preserved so
// point representation changes do not require a new C++ function prototype.
swipegate_on_swipe_process_hook:
    sub sp, sp, #0x110

    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    str x8, [sp, #0x40]
    str x30, [sp, #0x48]

    stp q0, q1, [sp, #0x50]
    stp q2, q3, [sp, #0x70]
    stp q4, q5, [sp, #0x90]
    stp q6, q7, [sp, #0xb0]

    // Helper signature:
    // float swipegate_hook_enter_and_gate(uint32_t ready, uint32_t side,
    //                                     float horizontal_distance);
    // Integer and FP argument banks are independent in AAPCS64, so s0 already
    // carries the original horizontal distance while w1/w2 are copied to w0/w1.
    mov w0, w1
    mov w1, w2
    bl swipegate_hook_enter_and_gate
    str s0, [sp, #0xd0]

    // Restore the original call frame, then replace only the low scalar in v0.
    ldp q0, q1, [sp, #0x50]
    ldp q2, q3, [sp, #0x70]
    ldp q4, q5, [sp, #0x90]
    ldp q6, q7, [sp, #0xb0]
    ldr s0, [sp, #0xd0]

    ldp x0, x1, [sp, #0x00]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldr x8, [sp, #0x40]

    adrp x17, gSwipeGateOriginalOnSwipeProcess
    add x17, x17, :lo12:gSwipeGateOriginalOnSwipeProcess
    ldar x16, [x17]
    cbz x16, 1f
    blr x16
1:
    bl swipegate_hook_exit
    ldr x30, [sp, #0x48]
    add sp, sp, #0x110
    ret

.size swipegate_on_swipe_process_hook, .-swipegate_on_swipe_process_hook
