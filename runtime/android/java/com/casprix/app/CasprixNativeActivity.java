package com.casprix.app;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;

/**
 * CasprixNativeActivity — the smallest possible replacement for
 * {@code android.app.NativeActivity}.
 *
 * WHY THIS EXISTS: Android's {@code AccessibilityNodeProvider} has no NDK
 * equivalent and is only reachable through a {@code View} in the hierarchy that
 * overrides {@code getAccessibilityNodeProvider()}. {@code NativeActivity}'s
 * internal {@code SurfaceView} is package-private and cannot be subclassed or
 * swapped, so a {@code View} we control must be added — which requires an
 * {@code Activity} we control. See the report for the full reasoning.
 *
 * WHAT IT DOES NOT DO: it does not touch rendering, EGL, input, or lifecycle.
 * It extends {@code NativeActivity} verbatim, so the native library still
 * loads, {@code ANativeActivity_onCreate} still runs, the SurfaceView is still
 * created, and {@code AInputQueue} input still flows — all unchanged. The only
 * addition is one invisible overlay {@link A11yHostView} added on top of the
 * content frame.
 *
 * There is ZERO app logic here. No state, no widget knowledge.
 */
public final class CasprixNativeActivity extends NativeActivity {

    private A11yHostView a11yHost;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // The content frame that NativeActivity dropped its SurfaceView into.
        ViewGroup content = findViewById(android.R.id.content);
        if (content == null) {
            return; // extremely unlikely; degrade to plain NativeActivity behaviour
        }

        a11yHost = new A11yHostView(this);
        // Full-bleed, but transparent and non-interactive: it never draws and
        // never consumes touches — it exists only to publish the a11y tree.
        a11yHost.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        content.addView(a11yHost);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && a11yHost != null) {
            // Report the current surface size to the native bridge so the
            // placeholder root node has real bounds. Purely additive.
            View decor = getWindow().getDecorView();
            A11yNodeProvider.nativeSetSurfaceSize(decor.getWidth(), decor.getHeight());
        }
    }
}
