package io.github.pzhown.hyperos4swipegate;

import android.content.SharedPreferences;
import android.os.Bundle;

import androidx.annotation.Nullable;
import androidx.preference.Preference;

import fan.preference.PreferenceFragment;
import fan.preference.SeekBarPreferenceCompat;

public final class SettingsFragment extends PreferenceFragment {
    private static final String PREF_DP_MIGRATED = "threshold_dp_migrated_v1";

    private Preference statusPreference;

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        setPreferencesFromResource(R.xml.preferences_settings, rootKey);

        SeekBarPreferenceCompat threshold = findPreference(ConfigBridge.PREF_KEY_THRESHOLD_DP);
        statusPreference = findPreference("runtime_status");
        if (threshold == null || statusPreference == null) return;

        SharedPreferences prefs = getPreferenceManager().getSharedPreferences();
        int stored = migrateLegacyValueIfNeeded(prefs);
        threshold.setValue(stored);

        threshold.setOnPreferenceChangeListener((preference, newValue) -> {
            int value = newValue instanceof Number
                    ? ((Number) newValue).intValue()
                    : ConfigBridge.DEFAULT_THRESHOLD_DP;
            writeThresholdDp(value);
            return true;
        });

        writeThresholdDp(stored);
    }

    private int migrateLegacyValueIfNeeded(SharedPreferences prefs) {
        if (prefs.getBoolean(PREF_DP_MIGRATED, false)) {
            return prefs.getInt(
                    ConfigBridge.PREF_KEY_THRESHOLD_DP,
                    ConfigBridge.DEFAULT_THRESHOLD_DP);
        }

        int migrated = prefs.getInt(
                ConfigBridge.PREF_KEY_THRESHOLD_DP,
                ConfigBridge.DEFAULT_THRESHOLD_DP);

        // Pixel builds used trigger_threshold_px. Preserve the user's visual
        // distance by converting with the current Android display density.
        if (prefs.contains(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX)) {
            int legacyPx = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_THRESHOLD_PX, 0);
            if (legacyPx > 0) {
                float density = getResources().getDisplayMetrics().density;
                if (density > 0f) migrated = Math.round(legacyPx / density);
            } else {
                migrated = 0;
            }
        } else if (prefs.contains(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP)) {
            // A short-lived experimental build represented the slider as extra
            // distance beyond stock. Convert it to the new absolute dp value.
            int extraDp = prefs.getInt(ConfigBridge.LEGACY_PREF_KEY_EXTRA_DP, 0);
            migrated = extraDp <= 0 ? 0 : ConfigBridge.STOCK_THRESHOLD_DP + extraDp;
        }

        migrated = Math.max(0, Math.min(ConfigBridge.MAX_THRESHOLD_DP, migrated));
        prefs.edit()
                .putInt(ConfigBridge.PREF_KEY_THRESHOLD_DP, migrated)
                .putBoolean(PREF_DP_MIGRATED, true)
                .apply();
        return migrated;
    }

    private void writeThresholdDp(int value) {
        if (statusPreference != null) {
            statusPreference.setSummary(value == 0
                    ? getString(R.string.status_applying_default)
                    : getString(R.string.status_applying, value));
        }

        ConfigBridge.applyThresholdDpAsync(requireContext(), value, result -> {
            if (!isAdded() || statusPreference == null) return;

            if (!result.success()) {
                statusPreference.setSummary(getString(R.string.status_failed, result.message()));
                return;
            }

            if (result.value() == 0) {
                statusPreference.setSummary(getString(R.string.status_applied_default));
            } else if (result.value() <= ConfigBridge.STOCK_THRESHOLD_DP) {
                statusPreference.setSummary(
                        getString(R.string.status_applied_stock_floor, result.value()));
            } else {
                statusPreference.setSummary(getString(R.string.status_applied, result.value()));
            }
        });
    }
}
