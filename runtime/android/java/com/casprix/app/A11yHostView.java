package com.casprix.app;

import android.content.Context;
import android.view.MotionEvent;
import android.view.View;
import android.view.accessibility.AccessibilityNodeProvider;

/**
 * A11yHostView — an invisible, non-interactive {@link View} whose only job is
 * to return an {@link A11yNodeProvider} from
 * {@link #getAccessibilityNodeProvider()}. That is the single OS hook that
 * lets a NativeActivity app expose a virtual accessibility tree.
 *
 * It never draws (no {@code onDraw} override) and never handles touch
 * ({@link #onTouchEvent} returns {@code false}), so it is completely
 * transparent to the existing native render/input path.
 *
 * No app logic. No state beyond the lazily-created provider.
 */
final class A11yHostView extends View {

    private A11yNodeProvider provider;

    A11yHostView(Context context) {
        super(context);
        setWillNotDraw(true);
        setFocusable(false);
        setClickable(false);
        // We DO want this view itself to be "important for accessibility" so the
        // framework asks it for a provider; its virtual children carry the real
        // semantics.
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
    }

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        if (provider == null) {
            provider = new A11yNodeProvider(this);
        }
        return provider;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        return false; // let touches fall through to the SurfaceView below
    }
}
