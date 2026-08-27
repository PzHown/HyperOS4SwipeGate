package io.github.pzhown.hyperos4swipegate;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

public final class AboutFragment extends Fragment {
    private static final String REPOSITORY = "https://github.com/PzHown/HyperOS4SwipeGate";
    private static final String ISSUES = REPOSITORY + "/issues";

    public AboutFragment() {
        super(R.layout.fragment_about);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        TextView version = view.findViewById(R.id.about_version);
        version.setText(getString(R.string.about_version, BuildConfig.VERSION_NAME));

        view.findViewById(R.id.row_repository).setOnClickListener(v -> openUrl(REPOSITORY));
        view.findViewById(R.id.row_issues).setOnClickListener(v -> openUrl(ISSUES));
    }

    private void openUrl(String url) {
        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
    }
}
