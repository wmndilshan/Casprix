/*
 * a11y_bridge.h — Native accessibility query surface.
 *
 * Android's AccessibilityNodeProvider has NO NDK equivalent (verified: there is
 * no <android/accessibility*.h> in any NDK sysroot). A tiny Java shim
 * (CasprixNativeActivity + A11yHostView + A11yNodeProvider) implements the
 * framework-facing interface and forwards every call here via JNI.
 *
 * v1a: proved the JNI + packaging plumbing with a single hardcoded root node.
 * v1b (this file now): walks the live SGNode scene graph — real bounds,
 * labels, roles, flags, children, click actions, hit-testing — and fires a
 * TYPE_WINDOW_CONTENT_CHANGED up-call on structural changes.
 *
 * The JNI symbol surface (function names/signatures for
 * Java_com_casprix_app_A11yNodeProvider_*) is UNCHANGED from v1a, so the
 * already-verified Java shim needs no modification.
 *
 * If no scene-graph root is registered (cpx_a11y_set_root), every query falls
 * back to the v1a single-root-node behaviour so the shim still functions
 * before the app builds its tree.
 *
 * This file is compiled into libMainActivity.so alongside the existing pure-NDK
 * runtime. It adds NO dependency on the render/EGL/input path.
 */
#ifndef CPX_ANDROID_A11Y_BRIDGE_H
#define CPX_ANDROID_A11Y_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared so this header stays free of scene_graph.h. */
struct SGNode;

/* Virtual view id of the placeholder root. Matches HOST_VIEW-relative "root".
 * (AccessibilityNodeProvider.HOST_VIEW_ID == -1; children start at >= 0.) */
#define CPX_A11Y_ROOT_ID 0

/* Rectangle in screen pixels. */
typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} CpxA11yRect;

/* --- Called from the Java shim (JNI). Safe to call before any real UI. --- */

/* Number of virtual nodes currently exposed. v1a: always 1 (the root). */
int32_t cpx_a11y_node_count(void);

/* Virtual id of the node at DFS index, or -1 if out of range. v1a: 0 -> root. */
int32_t cpx_a11y_node_id_at(int32_t index);

/* Fill *out with the screen-space bounds of node `id`.
 * Returns 1 on success, 0 if `id` is unknown. v1a: root == full surface. */
int cpx_a11y_node_bounds(int32_t id, CpxA11yRect* out);

/* Accessibility label / content-description for `id`. Never NULL; may be "".
 * The returned pointer is owned by the bridge and stable until the next call
 * on the same thread. v1a: a fixed placeholder string for the root. */
const char* cpx_a11y_node_label(int32_t id);

/* Android class name that drives the TalkBack role, e.g.
 * "android.view.View", "android.widget.Button". v1a: "android.view.View". */
const char* cpx_a11y_node_class_name(int32_t id);

/* Packed flags for `id`. v1a: root is a plain, non-actionable container. */
#define CPX_A11Y_FLAG_FOCUSABLE   (1u << 0)
#define CPX_A11Y_FLAG_CLICKABLE   (1u << 1)
#define CPX_A11Y_FLAG_ENABLED     (1u << 2)
#define CPX_A11Y_FLAG_CHECKABLE   (1u << 3)
#define CPX_A11Y_FLAG_CHECKED     (1u << 4)
#define CPX_A11Y_FLAG_VISIBLE     (1u << 5)
uint32_t cpx_a11y_node_flags(int32_t id);

/* Parent virtual id of `id`, or -1 if `id` is the root. */
int32_t cpx_a11y_node_parent(int32_t id);

/* Number of children of `id`. v1a: root has 0. */
int32_t cpx_a11y_node_child_count(int32_t id);

/* Virtual id of the `index`-th child of `id`, or -1. v1a: none. */
int32_t cpx_a11y_node_child_at(int32_t id, int32_t index);

/* Perform an accessibility action on `id`.
 * `action` mirrors AccessibilityNodeInfo.ACTION_* (only CLICK == 0x10 and the
 * focus actions are meaningful in v1a). Returns 1 if handled, 0 otherwise.
 * v1a: nothing is actionable, always returns 0. */
int cpx_a11y_perform_action(int32_t id, int32_t action);

/* Hit-test a screen point to a virtual id, or -1. v1a: any in-bounds point
 * resolves to the root; out-of-bounds -> -1. */
int32_t cpx_a11y_hit_test(int32_t screen_x, int32_t screen_y);

/* Called by the Java shim after the native window exists (fallback root bounds
 * when no scene graph is registered). Purely additive. */
void cpx_a11y_set_surface_size(int32_t width_px, int32_t height_px);

/* --- Wiring from the runtime (not JNI) --- */

/* Register the current scene-graph root the bridge should walk. Called from
 * sg_app_set_root() (frame_loop). Passing NULL reverts to the v1a fallback. */
void cpx_a11y_set_root(struct SGNode* root);

/* Register the fixed screen origin of the scene-graph coordinate space so
 * SGNode.bounds (root-relative) can be converted to screen space. Defaults to
 * (0,0), which is correct for a full-bleed NativeActivity surface. */
void cpx_a11y_set_root_origin(int32_t screen_x, int32_t screen_y);

/* Registered by the JNI layer; invoked on a structural change so the Java
 * shim can post AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED. */
typedef void (*CpxA11yContentChangedFn)(void);
void cpx_a11y_set_content_changed_cb(CpxA11yContentChangedFn cb);

#ifdef __cplusplus
}
#endif

#endif /* CPX_ANDROID_A11Y_BRIDGE_H */
