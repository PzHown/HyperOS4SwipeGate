from pathlib import Path

p = Path('native/src/main.cpp')
text = p.read_text()
old = '''    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(\n            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;\n    const int64_t now = monotonicMs();\n    traceStockHapticCall(constant, now, callerReturnAddress);\n'''
new = '''    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(\n            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;\n    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;\n    const int64_t now = monotonicMs();\n    traceStockHapticCall(constant, now, callerReturnAddress);\n'''
count = text.count(old)
if count != 1:
    raise SystemExit(f'callsite insertion: expected 1 match, got {count}')
p.write_text(text.replace(old, new, 1))

Path('scripts/apply_release_callsite_compile_fix.py').unlink()
Path('.github/workflows/run-release-callsite-compile-fix.yml').unlink()
