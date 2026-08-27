package io.github.pzhown.hyperos4swipegate;

import android.os.Bundle;

import androidx.annotation.Nullable;
import androidx.preference.Preference;

import fan.preference.PreferenceFragment;
import fan.preference.SeekBarPreferenceCompat;

public final class SettingsFragment extends PreferenceFragment {
    private Preference statusPreference;

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        setPreferencesFromResource(R.xml.preferences_settings, rootKey);

        SeekBarPreferenceCompat threshold = findPreference(ConfigBridge.PREF_KEY_THRESHOLD);
        statusPreference = findPreference("runtime_status");
        if (threshold == null || statusPreference == null) {
            return;
        }

        threshold.setOnPreferenceChangeListener((preference, newValue) -> {
            int value = newValue instanceof Number
                    ? ((Number) newValue).intValue()
                    : ConfigBridge.DEFAULT_THRESHOLD;
            writeThreshold(value);
            return true;
        });

        int stored = getPreferenceManager()
                .getSharedPreferences()
                .getInt(ConfigBridge.PREF_KEY_THRESHOLD, ConfigBridge.DEFAULT_THRESHOLD);
        writeThreshold(stored);
    }

    private void writeThreshold(int value) {
        if (statusPreference != null) {
            statusPreference.setSummary(getString(R.string.status_applying, value));
        }
        ConfigBridge.applyThresholdAsync(requireContext(), value, result -> {
            if (!isAdded() || statusPreference == null) {
                return;
            }
            if (result.success()) {
                statusPreference.setSummary(getString(R.string.status_applied, result.value()));
            } else {
                statusPreference.setSummary(getString(R.string.status_failed, result.message()));
            }
        });
    }
}
