package io.github.pzhown.hyperos4swipegate;

import android.os.Bundle;

import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import fan.appcompat.app.AppCompatActivity;
import fan.miuixbase.widget.FilterSortTabView;
import fan.miuixbase.widget.FilterSortView2;

public final class MainActivity extends AppCompatActivity {
    private static final String STATE_TAB = "tab";
    private static final int TAB_SETTINGS = 0;
    private static final int TAB_ABOUT = 1;

    private FilterSortView2 tabBar;
    private int currentTab = TAB_SETTINGS;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        setTitle(R.string.app_name);

        tabBar = findViewById(R.id.main_tabs);
        FilterSortTabView settingsTab = findViewById(R.id.tab_settings);
        FilterSortTabView aboutTab = findViewById(R.id.tab_about);

        if (savedInstanceState != null) {
            currentTab = savedInstanceState.getInt(STATE_TAB, TAB_SETTINGS);
        }

        settingsTab.setOnClickListener(v -> showTab(TAB_SETTINGS));
        aboutTab.setOnClickListener(v -> showTab(TAB_ABOUT));
        tabBar.setFilteredTab(currentTab);

        if (savedInstanceState == null) {
            showTab(currentTab);
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putInt(STATE_TAB, currentTab);
        super.onSaveInstanceState(outState);
    }

    private void showTab(int tab) {
        currentTab = tab;
        tabBar.setFilteredTab(tab);
        Fragment fragment = tab == TAB_SETTINGS ? new SettingsFragment() : new AboutFragment();
        getSupportFragmentManager()
                .beginTransaction()
                .replace(R.id.content_container, fragment)
                .commit();
    }
}
