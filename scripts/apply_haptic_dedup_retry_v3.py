from pathlib import Path

old_script = Path(__file__).with_name("apply_haptic_dedup_retry.py")
source = old_script.read_text()
marker = "    (\n'''            \"HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-worker-only"
start = source.find(marker)
if start < 0:
    raise SystemExit("old policy tuple not found")
end = source.find("\n]", start)
if end < 0:
    raise SystemExit("replacements list end not found")
corrected = """    (
'''    logLine(ANDROID_LOG_INFO,
            \"HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved arc-max-age-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex\",
            static_cast<long long>(kHapticArcMaxAgeMs));''',
'''    logLine(ANDROID_LOG_INFO,
            \"HAPTIC_V2 enabled policy=worker-only-loaded-elf-import ext-only constant=0 tagged-arc-preserved process-lifetime-arc retry-on-miss=1 confirm-dedup-ms=%lld first-segment-only no-module-second no-module-commit no-dlsym no-dlopen no-hook-mutex\",
            static_cast<long long>(kHapticConfirmSuppressMs));'''),
"""
source = source[:start] + corrected + source[end:]
namespace = {"__name__": "__main__", "__file__": str(old_script)}
exec(compile(source, str(old_script), "exec"), namespace)
