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

        SeekBarPreferenceCompat extraDistance = findPreference(ConfigBridge.PREF_KEY_EXTRA_DP);
        statusPreference = findPreference("runtime_status");
        if (extraDistance == null || statusPreference == null) {
            return;
        }

        extraDistance.setOnPreferenceChangeListener((preference, newValue) -> {
            int value = newValue instanceof Number
                    ? ((Number) newValue).intValue()
                    : ConfigBridge.DEFAULT_EXTRA_DP;
            writeExtraDp(value);
            return true;
        });

        int stored = getPreferenceManager()
                .getSharedPreferences()
                .getInt(ConfigBridge.PREF_KEY_EXTRA_DP, ConfigBridge.DEFAULT_EXTRA_DP);
        writeExtraDp(stored);
    }

    private void writeExtraDp(int value) {
        if (statusPreference != null) {
            statusPreference.setSummary(value == 0
                    ? getString(R.string.status_applying_default)
                    : getString(R.string.status_applying, value));
        }
        ConfigBridge.applyExtraDpAsync(requireContext(), value, result -> {
            if (!isAdded() || statusPreference == null) {
                return;
            }
            if (result.success()) {
                statusPreference.setSummary(result.value() == 0
                        ? getString(R.string.status_applied_default)
                        : getString(R.string.status_applied, result.value()));
            } else {
                statusPreference.setSummary(getString(R.string.status_failed, result.message()));
            }
        });
    }
}
