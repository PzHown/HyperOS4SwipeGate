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
\n\n// Haptic lifecycle hooks. Preserve opaque call arguments and forward to the LSPosed trampoline.\n.macro SWIPEGATE_EVENT_HOOK name, helper, original\n.global \name\n.hidden \name\n.type \name, %function\n\name:\n    sub sp, sp, #0x110\n    stp x0, x1, [sp, #0x00]\n    stp x2, x3, [sp, #0x10]\n    stp x4, x5, [sp, #0x20]\n    stp x6, x7, [sp, #0x30]\n    str x8, [sp, #0x40]\n    str x30, [sp, #0x48]\n    stp q0, q1, [sp, #0x50]\n    stp q2, q3, [sp, #0x70]\n    stp q4, q5, [sp, #0x90]\n    stp q6, q7, [sp, #0xb0]\n    bl \helper\n    ldp q0, q1, [sp, #0x50]\n    ldp q2, q3, [sp, #0x70]\n    ldp q4, q5, [sp, #0x90]\n    ldp q6, q7, [sp, #0xb0]\n    ldp x0, x1, [sp, #0x00]\n    ldp x2, x3, [sp, #0x10]\n    ldp x4, x5, [sp, #0x20]\n    ldp x6, x7, [sp, #0x30]\n    ldr x8, [sp, #0x40]\n    adrp x17, \original\n    add x17, x17, :lo12:\original\n    ldar x16, [x17]\n    cbz x16, 9f\n    blr x16\n9:\n    ldr x30, [sp, #0x48]\n    add sp, sp, #0x110\n    ret\n.size \name, .-\name\n.endm\n\nSWIPEGATE_EVENT_HOOK swipegate_on_swipe_start_hook, swipegate_haptic_on_swipe_start, gSwipeGateOriginalOnSwipeStart\nSWIPEGATE_EVENT_HOOK swipegate_on_back_invoke_hook, swipegate_haptic_on_back_invoke, gSwipeGateOriginalOnBackInvoke\nSWIPEGATE_EVENT_HOOK swipegate_on_back_cancelled_hook, swipegate_haptic_on_back_cancelled, gSwipeGateOriginalOnBackCancelled\n