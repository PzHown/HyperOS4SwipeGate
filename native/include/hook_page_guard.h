#pragma once

#include "native_api.h"

// Installs an LSPosed inline hook only after the HyperRT madvise import has
// been guarded against MADV_DONTNEED. Returns non-zero and does not hook
// when the guard is not ready (fail closed).
int swipegate_install_protected_inline_hook(
        HookFunType hookFunction,
        void *target,
        void *replacement,
        void **backup);

// Opportunistic retry used by the existing watchdog/loader paths.
bool swipegate_page_guard_ready();
