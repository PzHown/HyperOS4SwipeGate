    .text
    .p2align 2
    .globl SwipeGateBroadcastSendCaptureHook
    .hidden SwipeGateBroadcastSendCaptureHook
    .type SwipeGateBroadcastSendCaptureHook, %function
SwipeGateBroadcastSendCaptureHook:
    // hyper_os_broadcast_private::scene::impls::send_broadcast receives the Intent in x0
    // and a pointer to Shared<Runtime> in x1. Its Rust Result is returned indirectly via x8.
    // Preserve every argument plus x8/LR, capture only the runtime holder, then tail-call the
    // original implementation so Xiaomi's ABI and ownership remain untouched.
    bti c
    sub sp, sp, #0x60
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x30, [sp, #0x40]
    ldr x0, [sp, #0x08]
    bl SwipeGateCaptureBroadcastRuntime
    ldp x0, x1, [sp, #0x00]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x30, [sp, #0x40]
    adrp x16, g_swipegate_original_broadcast_send
    add x16, x16, :lo12:g_swipegate_original_broadcast_send
    ldar x16, [x16]
    add sp, sp, #0x60
    br x16
    .size SwipeGateBroadcastSendCaptureHook, .-SwipeGateBroadcastSendCaptureHook

    .section .note.gnu.property,"a",%note
    .p2align 3
    .long 4
    .long 16
    .long 5
    .asciz "GNU"
    .long 0xc0000000
    .long 4
    .long 7
    .long 0

    .section .note.GNU-stack,"",%progbits
