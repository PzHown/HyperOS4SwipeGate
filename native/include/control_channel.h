#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Live configuration received through SystemUI -> Xiaomi fsgesture -> HyOS Runtime. A negative
// value means no authenticated runtime value has arrived yet and callers should fall back to the
// persisted launcher cache / legacy property path.
int swipegate_control_threshold_dp();
int swipegate_control_log_level();
int swipegate_control_haptic_enabled();
// Request one haptic pulse through the authenticated HyOS Runtime -> SystemUI bridge.
// kind: 0 = threshold/ready tick, 1 = committed back action. Returns 1 when queued.
int swipegate_control_request_haptic(int kind);

// Feed every Native log line into the runtime control plane. Hook state is parsed regardless of
// the user-facing App log level; App log retention remains controlled by log_level.
void swipegate_control_on_log(int priority, const char *text);

// Ensure the process-local HyOS Runtime bridge installer is active. There is no socket and no
// network transport; this function only drives the native broadcast bridge lifecycle.
void swipegate_control_sync_if_due();

#ifdef __cplusplus
}
#endif
