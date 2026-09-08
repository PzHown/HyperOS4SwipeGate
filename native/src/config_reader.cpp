#include "control_channel.h"

#include <sys/system_properties.h>

#include <cstdio>
#include <cstring>

extern "C" int __real___system_property_get(const char *name, char *value);

extern "C" int __wrap___system_property_get(const char *name, char *value) {
    if (name != nullptr && value != nullptr
            && std::strcmp(name, "persist.hyperos4swipegate.threshold_dp") == 0) {
        // The control plane restores the complete configuration independently
        // from the App. Legacy cache migration lives in RuntimeConfigStore.
        swipegate_control_sync_if_due();
        const int threshold = swipegate_control_threshold_dp();
        if (threshold >= 88 && threshold <= 300) {
            const int written = std::snprintf(value, PROP_VALUE_MAX, "%d", threshold);
            return written > 0 ? written : 0;
        }
    }
    return __real___system_property_get(name, value);
}
