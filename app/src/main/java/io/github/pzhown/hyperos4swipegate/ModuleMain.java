package io.github.pzhown.hyperos4swipegate;

import io.github.libxposed.api.XposedModule;

/**
 * Modern libxposed metadata entry. HyperOS 4 Launcher 8.x is a Rust/native
 * hyos_spawner child, so the active hook implementation is native_init.
 */
public final class ModuleMain extends XposedModule {
    public ModuleMain() {
        super();
    }
}
