package io.github.libxposed.api;
public class XposedModule {
 public void onModuleLoaded(XposedModuleInterface.ModuleLoadedParam param) {}
 protected void log(int priority,String tag,String message) {}
 protected void log(int priority,String tag,String message,Throwable error) {}
}
