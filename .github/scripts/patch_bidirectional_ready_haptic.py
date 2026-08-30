from pathlib import Path

p = Path('native/src/main.cpp')
s = p.read_text()
old = '''    const bool readyNow = readyFinish != 0;
    const bool readyBefore = gReadyHapticLatched.exchange(readyNow, std::memory_order_acq_rel);
    if (readyNow && !readyBefore) performReturnHaptic("ready", true);
    return gateHorizontalDistance(readyFinish != 0, side, horizontalDistancePx);
'''
new = '''    const bool readyNow = readyFinish != 0;
    const bool readyBefore = gReadyHapticLatched.exchange(readyNow, std::memory_order_acq_rel);
    if (readyNow && !readyBefore) {
        performReturnHaptic("ready-enter", true);
    } else if (!readyNow && readyBefore && gGestureActive.load(std::memory_order_acquire)) {
        performReturnHaptic("ready-exit", true);
    }
    return gateHorizontalDistance(readyFinish != 0, side, horizontalDistancePx);
'''
count = s.count(old)
if count != 1:
    raise SystemExit(f'ready transition block mismatch: {count}')
p.write_text(s.replace(old, new, 1))
