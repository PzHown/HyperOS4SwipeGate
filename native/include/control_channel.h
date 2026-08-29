#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Live configuration received directly from the SwipeGate app. A negative value means that no
// live value has been received yet and callers should fall back to the persisted launcher cache.
int swipegate_control_threshold_dp();
int swipegate_control_log_level();

// Feed every native log line into the control plane. Hook state is always parsed regardless of the
// user-facing App log level; App log retention itself remains controlled by log_level.
void swipegate_control_on_log(int priority, const char *text);

// Opportunistically exchange hook state/logs for the latest App configuration. This is intentionally
// outbound-only from the Launcher process so no worker thread is ever created in the root
// hyos_spawner before fork.
void swipegate_control_sync_if_due();

#ifdef __cplusplus
}
#endif
