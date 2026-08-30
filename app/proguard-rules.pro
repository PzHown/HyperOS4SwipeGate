# Keep code/resource shrinking enabled, but do not rename classes or members.
-dontobfuscate

# LSPosed loads Java entrypoints by exact class name from META-INF/xposed/java_init.list.
-keep class io.github.pzhown.hyperos4swipegate.ModuleMain { *; }
-keep class io.github.pzhown.hyperos4swipegate.NativeErrorReporter { *; }
-keep class io.github.pzhown.hyperos4swipegate.SystemUiBridgeModule { *; }
