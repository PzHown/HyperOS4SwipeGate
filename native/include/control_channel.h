#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Launcher-local configuration: restored from persistent storage on initialization,
// then updated by authenticated SystemUI carriers. No App heartbeat is required.
// A negative result means this is not yet an initialized Launcher process.
int swipegate_control_threshold_dp();
int swipegate_control_log_level();
int swipegate_control_haptic_enabled();
int swipegate_control_break_open_enabled();

// Feed every Native log line into the runtime control plane. Hook state is parsed regardless of
// the user-facing App log level; App log retention remains controlled by log_level.
void swipegate_control_on_log(int priority, const char *text);

// Restore local configuration and ensure the process-local broadcast bridge installer.
// This does not wake the App, open a socket, or fetch App preferences.
void swipegate_control_sync_if_due();

#ifdef __cplusplus
}
#endif
