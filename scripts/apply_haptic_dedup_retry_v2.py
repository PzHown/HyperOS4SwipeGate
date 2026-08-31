from pathlib import Path

old_script = Path(__file__).with_name("apply_haptic_dedup_retry.py")
source = old_script.read_text()
old = '''    (\n'''            \"HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-worker-only ext-only constant=0 tagged-arc arc-max-age=300000ms first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex\");''',\n'''            \"HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-worker-only ext-only constant=0 tagged-arc process-lifetime-arc retry-on-miss=1 confirm-dedup=120ms first-segment-only no-module-second no-module-commit no-apk-open no-haptic-dlopen no-hook-mutex\");'''),\n'''
new = '''    (\n'''    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved arc-max-age-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex\",\n            static_cast<long long>(kHapticArcMaxAgeMs));''',\n'''    logLine(ANDROID_LOG_INFO,\n            \"HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved process-lifetime-arc retry-on-miss=1 confirm-dedup-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex\",\n            static_cast<long long>(kHapticConfirmSuppressMs));'''),\n'''
if old not in source:
    raise SystemExit("failed to repair expected log replacement in v1 patch script")
source = source.replace(old, new, 1)
namespace = {"__name__": "__main__", "__file__": str(old_script)}
exec(compile(source, str(old_script), "exec"), namespace)
