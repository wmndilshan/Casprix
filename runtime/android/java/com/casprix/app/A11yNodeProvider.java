package com.casprix.app;

import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

import java.lang.ref.WeakReference;
import java.util.List;

/**
 * A11yNodeProvider — the {@link AccessibilityNodeProvider} implementation.
 *
 * It contains NO business logic and NO widget knowledge. Every question the
 * accessibility framework asks is answered by forwarding to native functions
 * in {@code runtime/android/a11y_bridge.c} via the {@code native*} methods
 * declared at the bottom of this class.
 *
 * v1a: the native side returns a single hardcoded root node covering the whole
 * surface. This class is already written to walk an arbitrary virtual tree
 * (children, parent, bounds, label, role, actions) so that v1b only has to make
 * the native side return real data — no Java changes.
 */
final class A11yNodeProvider extends AccessibilityNodeProvider {

    /** Mirrors CPX_A11Y_ROOT_ID in a11y_bridge.h. */
    private static final int ROOT_ID = 0;

    // Mirrors the CPX_A11Y_FLAG_* bits in a11y_bridge.h.
    private static final int FLAG_FOCUSABLE = 1 << 0;
    private static final int FLAG_CLICKABLE = 1 << 1;
    private static final int FLAG_ENABLED   = 1 << 2;
    private static final int FLAG_CHECKABLE = 1 << 3;
    private static final int FLAG_CHECKED   = 1 << 4;
    private static final int FLAG_VISIBLE   = 1 << 5;

    private final View host;

    /* Weak ref to the current host so the native content-changed up-call
     * (a11y_bridge.c -> onNativeContentChanged) can post an event without
     * holding a strong reference to a View. */
    private static WeakReference<View> sHostRef = new WeakReference<>(null);
    private static final Handler sMain = new Handler(Looper.getMainLooper());
    /* Debounce: coalesce bursts into one TYPE_WINDOW_CONTENT_CHANGED. */
    private static long sLastEventMs = 0L;
    private static boolean sPending = false;
    private static final long MIN_INTERVAL_MS = 100L;

    A11yNodeProvider(View host) {
        this.host = host;
        sHostRef = new WeakReference<>(host);
    }

    /**
     * Invoked from native (a11y_bridge.c) when the scene graph's structure or a
     * label changes — NOT on paint/layout. Posts a single, debounced
     * {@link AccessibilityEvent#TYPE_WINDOW_CONTENT_CHANGED} on the host view.
     * If no host is attached this is a no-op.
     */
    @SuppressWarnings("unused") // called via JNI
    static void onNativeContentChanged() {
        final View host = sHostRef.get();
        if (host == null) return;
        synchronized (A11yNodeProvider.class) {
            if (sPending) return;
            sPending = true;
        }
        sMain.post(new Runnable() {
            @Override public void run() {
                synchronized (A11yNodeProvider.class) { sPending = false; }
                long now = SystemClock.uptimeMillis();
                if (now - sLastEventMs < MIN_INTERVAL_MS) {
                    sMain.postDelayed(this, MIN_INTERVAL_MS);
                    synchronized (A11yNodeProvider.class) { sPending = true; }
                    return;
                }
                sLastEventMs = now;
                host.sendAccessibilityEvent(
                        AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            }
        });
    }

    @Override
    public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
        if (virtualViewId == View.NO_ID) {
            // The host view node: it just parents every native virtual node.
            AccessibilityNodeInfo hostInfo =
                    AccessibilityNodeInfo.obtain(host);
            host.onInitializeAccessibilityNodeInfo(hostInfo);
            int count = nativeNodeCount();
            for (int i = 0; i < count; i++) {
                int childId = nativeNodeIdAt(i);
                if (childId >= 0) {
                    hostInfo.addChild(host, childId);
                }
            }
            return hostInfo;
        }

        int[] b = nativeNodeBounds(virtualViewId);
        if (b == null) {
            return null; // unknown virtual id
        }

        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(host, virtualViewId);
        info.setPackageName(host.getContext().getPackageName());
        info.setClassName(nativeNodeClassName(virtualViewId));
        info.setText(nativeNodeLabel(virtualViewId));
        info.setContentDescription(nativeNodeLabel(virtualViewId));

        // Bounds. Native reports screen space; the framework wants both.
        Rect screen = new Rect(b[0], b[1], b[2], b[3]);
        info.setBoundsInScreen(screen);

        int flags = nativeNodeFlags(virtualViewId);
        info.setEnabled((flags & FLAG_ENABLED) != 0);
        info.setVisibleToUser((flags & FLAG_VISIBLE) != 0);
        info.setFocusable((flags & FLAG_FOCUSABLE) != 0);
        info.setClickable((flags & FLAG_CLICKABLE) != 0);
        info.setCheckable((flags & FLAG_CHECKABLE) != 0);
        info.setChecked((flags & FLAG_CHECKED) != 0);

        if ((flags & FLAG_CLICKABLE) != 0) {
            info.addAction(AccessibilityNodeInfo.ACTION_CLICK);
        }
        info.addAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
        info.addAction(AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS);

        // Parent / children.
        int parentId = nativeNodeParent(virtualViewId);
        if (parentId < 0) {
            info.setParent(host); // root's parent is the host view
        } else {
            info.setParent(host, parentId);
        }
        int childCount = nativeNodeChildCount(virtualViewId);
        for (int i = 0; i < childCount; i++) {
            int childId = nativeNodeChildAt(virtualViewId, i);
            if (childId >= 0) {
                info.addChild(host, childId);
            }
        }
        return info;
    }

    @Override
    public boolean performAction(int virtualViewId, int action, Bundle arguments) {
        switch (action) {
            case AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS:
            case AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:
                // The framework tracks a11y focus itself; nothing native to do
                // in v1a. Report handled so exploration works.
                return true;
            default:
                return nativePerformAction(virtualViewId, action);
        }
    }

    @Override
    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(
            String text, int virtualViewId) {
        // v1a: no text index. Return empty (not null) — the framework tolerates
        // an empty result. v1b can back this with a native substring scan.
        return java.util.Collections.emptyList();
    }

    // ---- native forwarders (implemented in runtime/android/a11y_bridge.c) ---

    static native int      nativeNodeCount();
    static native int      nativeNodeIdAt(int index);
    static native int[]    nativeNodeBounds(int id);        // {l,t,r,b} or null
    static native String   nativeNodeLabel(int id);
    static native String   nativeNodeClassName(int id);
    static native int      nativeNodeFlags(int id);
    static native int      nativeNodeParent(int id);
    static native int      nativeNodeChildCount(int id);
    static native int      nativeNodeChildAt(int id, int index);
    static native boolean  nativePerformAction(int id, int action);
    static native int      nativeHitTest(int x, int y);
    static native void     nativeSetSurfaceSize(int w, int h);
}
