/*
 * a11y_bridge.c — Native accessibility query surface (v1b).
 *
 * Walks the live SGNode scene graph registered via cpx_a11y_set_root() and
 * answers every AccessibilityNodeProvider query the Java shim forwards.
 *
 *   - node enumeration  : pre-order DFS over *surfaced* nodes
 *   - a node is surfaced if: SG_VISIBLE, not a11y_hidden, and it is either
 *       interactive (SG_INTERACTIVE), has an explicit a11y_role != NONE, or is
 *       a non-empty SG_NODE_TEXT.
 *   - a11y_hidden / non-surfaced nodes are TRANSPARENT: their surfaced
 *       descendants re-parent to the nearest surfaced ancestor.
 *   - virtual id == SGNode.id (unique, starts at 1). id 0 is reserved for the
 *       v1a fallback root used only when no scene graph is registered.
 *   - bounds: SGNode.bounds (root-relative absolute) + the fixed root origin.
 *   - click: sg_node_emit(node, SG_EVENT_CLICK, NULL) — the hardened dispatch
 *       path (NULL-handler safe, depth-capped), NOT a bypass.
 *   - hit-test: sg_hit_test(), then walk up to the nearest surfaced node.
 *
 * The JNI symbol surface is IDENTICAL to v1a.
 */
#include "a11y_bridge.h"

#include <string.h>
#include <stdio.h>

/* The bridge is compiled into libMainActivity.so together with the scene graph;
 * pull the real definitions in. On a host unit build this resolves against the
 * same runtime/skia sources linked into the test. */
#include "scene_graph.h"
#include "widgets.h"
#include "events.h"

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

static SGNode*  g_root       = NULL;
static int32_t  g_origin_x   = 0;
static int32_t  g_origin_y   = 0;

/* v1a fallback surface size (used only when g_root == NULL). */
static int32_t  g_surface_w  = 0;
static int32_t  g_surface_h  = 0;

/* v1a fallback root id + label. */
#define CPX_A11Y_FALLBACK_ROOT_ID  0
static const char* const CPX_A11Y_FALLBACK_LABEL =
    "Casprix application (accessibility bridge active, no screen mounted)";

/* Returned-string scratch (per the header contract: valid until the next call
 * on the same thread). */
static char g_label_buf[512];

static CpxA11yContentChangedFn g_content_changed_cb = NULL;

/* ------------------------------------------------------------------------- */
/* Wiring entry points                                                        */
/* ------------------------------------------------------------------------- */

void cpx_a11y_set_root(SGNode* root) {
    g_root = root;
}

void cpx_a11y_set_root_origin(int32_t screen_x, int32_t screen_y) {
    g_origin_x = screen_x;
    g_origin_y = screen_y;
}

void cpx_a11y_set_surface_size(int32_t width_px, int32_t height_px) {
    if (width_px  >= 0) g_surface_w = width_px;
    if (height_px >= 0) g_surface_h = height_px;
}

void cpx_a11y_set_content_changed_cb(CpxA11yContentChangedFn cb) {
    g_content_changed_cb = cb;
}

/* Bounce a scene-graph structural change out to the Java shim. Registered with
 * the scene graph by whoever owns the tree (see android_runtime / frame_loop
 * wiring). Kept here so the JNI up-call and the C hook live together. */
static void a11y_on_structural_change(void* user_data) {
    (void)user_data;
    if (g_content_changed_cb) {
        g_content_changed_cb();
    }
}

/* Called by the runtime once, to connect the scene-graph hook to this bridge.
 * Idempotent. */
void cpx_a11y_install_scene_hook(void) {
    sg_a11y_set_structural_change_cb(a11y_on_structural_change, NULL);
}

/* ------------------------------------------------------------------------- */
/* Traversal helpers                                                          */
/* ------------------------------------------------------------------------- */

static int node_has_text(const SGNode* n) {
    return n && n->type == SG_NODE_TEXT &&
           n->data.text.text && n->data.text.text[0] != '\0';
}

/* A node the accessibility framework should see as its own element. */
static int node_is_surfaced(const SGNode* n) {
    if (!n) return 0;
    if (!(n->flags & SG_VISIBLE)) return 0;
    if (n->a11y_hidden) return 0;
    if (n->a11y_role != SG_A11Y_ROLE_NONE) return 1;
    if (n->flags & SG_INTERACTIVE) return 1;
    if (node_has_text(n)) return 1;
    return 0;
}

/* Is this subtree completely invisible to a11y (so we can prune)? A node with
 * a11y_hidden still lets its children through, so only SG_VISIBLE gates the
 * whole subtree. */
static int subtree_pruned(const SGNode* n) {
    return !n || !(n->flags & SG_VISIBLE);
}

/* Find node by virtual id (== SGNode.id) via DFS from g_root. */
static SGNode* find_by_id(SGNode* n, int32_t id) {
    if (!n) return NULL;
    if ((int32_t)n->id == id) return n;
    for (SGNode* c = n->first_child; c; c = c->next_sibling) {
        SGNode* hit = find_by_id(c, id);
        if (hit) return hit;
    }
    return NULL;
}

/* Nearest surfaced ancestor of n (excluding n), or NULL. */
static SGNode* surfaced_ancestor(SGNode* n) {
    for (SGNode* p = n ? n->parent : NULL; p; p = p->parent) {
        if (node_is_surfaced(p)) return p;
    }
    return NULL;
}

/* Pre-order DFS visiting every surfaced node. cb returns non-zero to stop. */
typedef int (*VisitFn)(SGNode* n, void* ud);
static int visit_surfaced(SGNode* n, VisitFn cb, void* ud) {
    if (subtree_pruned(n)) return 0;
    if (node_is_surfaced(n)) {
        if (cb(n, ud)) return 1;
    }
    for (SGNode* c = n->first_child; c; c = c->next_sibling) {
        if (visit_surfaced(c, cb, ud)) return 1;
    }
    return 0;
}

/* ---- count ---- */
struct CountCtx { int32_t n; };
static int count_cb(SGNode* n, void* ud) {
    (void)n;
    ((struct CountCtx*)ud)->n++;
    return 0;
}

int32_t cpx_a11y_node_count(void) {
    if (!g_root) return 1;                     /* fallback: the single root */
    struct CountCtx ctx = { 0 };
    visit_surfaced(g_root, count_cb, &ctx);
    return ctx.n;
}

/* ---- id at DFS index ---- */
struct IndexCtx { int32_t target; int32_t cur; int32_t found; };
static int index_cb(SGNode* n, void* ud) {
    struct IndexCtx* c = (struct IndexCtx*)ud;
    if (c->cur == c->target) {
        c->found = (int32_t)n->id;
        return 1;
    }
    c->cur++;
    return 0;
}

int32_t cpx_a11y_node_id_at(int32_t index) {
    if (!g_root) return (index == 0) ? CPX_A11Y_FALLBACK_ROOT_ID : -1;
    if (index < 0) return -1;
    struct IndexCtx c = { index, 0, -1 };
    visit_surfaced(g_root, index_cb, &c);
    return c.found;
}

/* ---- bounds ---- */
int cpx_a11y_node_bounds(int32_t id, CpxA11yRect* out) {
    if (!out) return 0;

    if (!g_root) {
        if (id != CPX_A11Y_FALLBACK_ROOT_ID) return 0;
        out->left = 0; out->top = 0;
        out->right  = (g_surface_w > 0) ? g_surface_w : 1;
        out->bottom = (g_surface_h > 0) ? g_surface_h : 1;
        return 1;
    }

    SGNode* n = find_by_id(g_root, id);
    if (!n || !node_is_surfaced(n)) return 0;

    out->left   = g_origin_x + (int32_t)n->bounds.x;
    out->top    = g_origin_y + (int32_t)n->bounds.y;
    out->right  = out->left  + (int32_t)n->bounds.w;
    out->bottom = out->top   + (int32_t)n->bounds.h;
    return 1;
}

/* ---- label ---- */

/* First descendant text content (DFS), skipping hidden-but-still-descend
 * decorative wrappers. Used to derive a Button's label from its text child. */
static const char* first_descendant_text(SGNode* n) {
    if (!n) return NULL;
    for (SGNode* c = n->first_child; c; c = c->next_sibling) {
        if (!(c->flags & SG_VISIBLE)) continue;
        if (node_has_text(c)) return c->data.text.text;
        const char* deeper = first_descendant_text(c);
        if (deeper) return deeper;
    }
    return NULL;
}

const char* cpx_a11y_node_label(int32_t id) {
    if (!g_root) {
        return (id == CPX_A11Y_FALLBACK_ROOT_ID) ? CPX_A11Y_FALLBACK_LABEL : "";
    }

    SGNode* n = find_by_id(g_root, id);
    if (!n) return "";

    const char* src = NULL;
    if (n->a11y_label && n->a11y_label[0]) {
        src = n->a11y_label;
    } else if (node_has_text(n)) {
        src = n->data.text.text;
    } else {
        src = first_descendant_text(n);
    }
    if (!src) return "";

    strncpy(g_label_buf, src, sizeof(g_label_buf) - 1);
    g_label_buf[sizeof(g_label_buf) - 1] = '\0';
    return g_label_buf;
}

/* ---- class name / role ---- */

static WidgetType node_widget_type(const SGNode* n) {
    if (!n || !n->user_data) return WIDGET_NONE;
    return *(const WidgetType*)n->user_data;   /* every widget state starts
                                                  with a WidgetType field */
}

static SGA11yRole effective_role(const SGNode* n) {
    if (!n) return SG_A11Y_ROLE_NONE;
    if (n->a11y_role != SG_A11Y_ROLE_NONE) return n->a11y_role;
    switch (node_widget_type(n)) {
        case WIDGET_BUTTON:     return SG_A11Y_ROLE_BUTTON;
        case WIDGET_TEXT_INPUT: return SG_A11Y_ROLE_EDIT_TEXT;
        case WIDGET_CHECKBOX:   return SG_A11Y_ROLE_CHECKBOX;
        default: break;
    }
    if (node_has_text(n)) return SG_A11Y_ROLE_TEXT;
    return SG_A11Y_ROLE_NONE;
}

const char* cpx_a11y_node_class_name(int32_t id) {
    if (!g_root) return "android.view.View";
    SGNode* n = find_by_id(g_root, id);
    switch (effective_role(n)) {
        case SG_A11Y_ROLE_BUTTON:    return "android.widget.Button";
        case SG_A11Y_ROLE_EDIT_TEXT: return "android.widget.EditText";
        case SG_A11Y_ROLE_CHECKBOX:  return "android.widget.CheckBox";
        case SG_A11Y_ROLE_IMAGE:     return "android.widget.ImageView";
        case SG_A11Y_ROLE_TEXT:
        case SG_A11Y_ROLE_HEADER:    return "android.widget.TextView";
        case SG_A11Y_ROLE_NONE:
        default:                     return "android.view.View";
    }
}

/* ---- flags ---- */
uint32_t cpx_a11y_node_flags(int32_t id) {
    if (!g_root) {
        return (id == CPX_A11Y_FALLBACK_ROOT_ID)
               ? (CPX_A11Y_FLAG_ENABLED | CPX_A11Y_FLAG_VISIBLE)
               : 0u;
    }

    SGNode* n = find_by_id(g_root, id);
    if (!n) return 0u;

    uint32_t f = 0u;
    if (n->flags & SG_VISIBLE) f |= CPX_A11Y_FLAG_VISIBLE;

    int disabled = (n->state_flags & SG_STATE_DISABLED) != 0;
    if (!disabled) f |= CPX_A11Y_FLAG_ENABLED;

    SGA11yRole role = effective_role(n);
    int interactive = (n->flags & SG_INTERACTIVE) != 0;

    if ((role == SG_A11Y_ROLE_BUTTON || interactive) && !disabled) {
        f |= CPX_A11Y_FLAG_CLICKABLE;
    }
    if ((role == SG_A11Y_ROLE_BUTTON || role == SG_A11Y_ROLE_EDIT_TEXT ||
         role == SG_A11Y_ROLE_CHECKBOX || interactive) && !disabled) {
        f |= CPX_A11Y_FLAG_FOCUSABLE;
    }
    if (role == SG_A11Y_ROLE_CHECKBOX) {
        f |= CPX_A11Y_FLAG_CHECKABLE;
        if (n->state_flags & SG_STATE_CHECKED) f |= CPX_A11Y_FLAG_CHECKED;
    }
    return f;
}

/* ---- parent / children ---- */
int32_t cpx_a11y_node_parent(int32_t id) {
    if (!g_root) return -1;
    SGNode* n = find_by_id(g_root, id);
    if (!n) return -1;
    SGNode* p = surfaced_ancestor(n);
    return p ? (int32_t)p->id : -1;     /* -1 => Java parents it to host view */
}

/* Collect the surfaced descendants whose nearest surfaced ancestor is `parent`
 * (i.e. skip through hidden/non-surfaced intermediates). */
struct ChildCtx { SGNode* parent; int32_t want_index; int32_t seen; int32_t found; };
static void collect_children(SGNode* n, SGNode* parent, struct ChildCtx* ctx) {
    for (SGNode* c = n->first_child; c; c = c->next_sibling) {
        if (subtree_pruned(c)) continue;
        if (node_is_surfaced(c)) {
            if (ctx->want_index < 0) {
                ctx->seen++;                       /* counting mode */
            } else if (ctx->seen == ctx->want_index) {
                ctx->found = (int32_t)c->id;
                return;
            } else {
                ctx->seen++;
            }
            /* do NOT descend past a surfaced child — its own children belong
             * to it, not to `parent`. */
        } else {
            collect_children(c, parent, ctx);      /* transparent — descend */
            if (ctx->found >= 0) return;
        }
    }
    (void)parent;
}

int32_t cpx_a11y_node_child_count(int32_t id) {
    if (!g_root) return 0;
    SGNode* n = find_by_id(g_root, id);
    if (!n || !node_is_surfaced(n)) return 0;
    struct ChildCtx ctx = { n, -1, 0, -1 };
    collect_children(n, n, &ctx);
    return ctx.seen;
}

int32_t cpx_a11y_node_child_at(int32_t id, int32_t index) {
    if (!g_root || index < 0) return -1;
    SGNode* n = find_by_id(g_root, id);
    if (!n || !node_is_surfaced(n)) return -1;
    struct ChildCtx ctx = { n, index, 0, -1 };
    collect_children(n, n, &ctx);
    return ctx.found;
}

/* ---- actions ---- */
#define CPX_A11Y_ACTION_CLICK 0x00000010   /* AccessibilityNodeInfo.ACTION_CLICK */

int cpx_a11y_perform_action(int32_t id, int32_t action) {
    if (!g_root) return 0;
    SGNode* n = find_by_id(g_root, id);
    if (!n) return 0;

    if (action == CPX_A11Y_ACTION_CLICK) {
        if (n->state_flags & SG_STATE_DISABLED) return 0;
        if (!(n->flags & SG_INTERACTIVE)) return 0;
        /* Reuse the hardened dispatch path (NULL-handler safe, depth-capped). */
        sg_node_emit(n, SG_EVENT_CLICK, NULL);
        return 1;
    }
    return 0;
}

/* ---- hit test ---- */
int32_t cpx_a11y_hit_test(int32_t screen_x, int32_t screen_y) {
    if (!g_root) {
        int32_t w = (g_surface_w > 0) ? g_surface_w : 1;
        int32_t h = (g_surface_h > 0) ? g_surface_h : 1;
        if (screen_x < 0 || screen_y < 0 || screen_x >= w || screen_y >= h) {
            return -1;
        }
        return CPX_A11Y_FALLBACK_ROOT_ID;
    }

    float local_x = (float)(screen_x - g_origin_x);
    float local_y = (float)(screen_y - g_origin_y);
    SGNode* hit = sg_hit_test(g_root, local_x, local_y);
    if (!hit) return -1;

    if (node_is_surfaced(hit)) return (int32_t)hit->id;
    SGNode* up = surfaced_ancestor(hit);
    return up ? (int32_t)up->id : -1;
}

/* ========================================================================= */
/* JNI wrappers — UNCHANGED from v1a (names + signatures).                     */
/* Class: com.casprix.app.A11yNodeProvider                                    */
/* ========================================================================= */

#ifdef __ANDROID__

#include <jni.h>
#include <android/log.h>

#define CPX_A11Y_TAG "CasprixA11y"

/* Cached VM + weak ref to the A11yHostView so the structural-change hook can
 * post an event without a JNIEnv in hand. Registered from the Java side. */
static JavaVM*  g_vm            = NULL;
static jclass   g_provider_cls  = NULL;   /* global ref */
static jmethodID g_notify_mid   = NULL;

static void jni_content_changed(void) {
    if (!g_vm || !g_provider_cls || !g_notify_mid) return;
    JNIEnv* env = NULL;
    int attached = 0;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, g_provider_cls, g_notify_mid);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeCount(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_count();
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeIdAt(JNIEnv* env, jclass cls,
                                                     jint index) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_id_at((int32_t)index);
}

/* Returns int[4] {left, top, right, bottom}, or null on unknown id. */
JNIEXPORT jintArray JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeBounds(JNIEnv* env, jclass cls,
                                                       jint id) {
    (void)cls;
    CpxA11yRect r;
    if (!cpx_a11y_node_bounds((int32_t)id, &r)) return NULL;
    jintArray arr = (*env)->NewIntArray(env, 4);
    if (!arr) return NULL;
    jint vals[4] = { r.left, r.top, r.right, r.bottom };
    (*env)->SetIntArrayRegion(env, arr, 0, 4, vals);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeLabel(JNIEnv* env, jclass cls,
                                                      jint id) {
    (void)cls;
    return (*env)->NewStringUTF(env, cpx_a11y_node_label((int32_t)id));
}

JNIEXPORT jstring JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeClassName(JNIEnv* env, jclass cls,
                                                          jint id) {
    (void)cls;
    return (*env)->NewStringUTF(env, cpx_a11y_node_class_name((int32_t)id));
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeFlags(JNIEnv* env, jclass cls,
                                                      jint id) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_flags((int32_t)id);
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeParent(JNIEnv* env, jclass cls,
                                                       jint id) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_parent((int32_t)id);
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeChildCount(JNIEnv* env, jclass cls,
                                                           jint id) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_child_count((int32_t)id);
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeNodeChildAt(JNIEnv* env, jclass cls,
                                                        jint id, jint index) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_node_child_at((int32_t)id, (int32_t)index);
}

JNIEXPORT jboolean JNICALL
Java_com_casprix_app_A11yNodeProvider_nativePerformAction(JNIEnv* env, jclass cls,
                                                          jint id, jint action) {
    (void)env; (void)cls;
    return cpx_a11y_perform_action((int32_t)id, (int32_t)action) ? JNI_TRUE
                                                                 : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeHitTest(JNIEnv* env, jclass cls,
                                                    jint x, jint y) {
    (void)env; (void)cls;
    return (jint)cpx_a11y_hit_test((int32_t)x, (int32_t)y);
}

JNIEXPORT void JNICALL
Java_com_casprix_app_A11yNodeProvider_nativeSetSurfaceSize(JNIEnv* env, jclass cls,
                                                           jint w, jint h) {
    (void)cls;
    cpx_a11y_set_surface_size((int32_t)w, (int32_t)h);

    /* First real call from the shim: wire the structural-change hook and cache
     * the JVM + provider class for the content-changed up-call. Safe to run
     * every time. */
    if (!g_vm) (*env)->GetJavaVM(env, &g_vm);
    if (!g_provider_cls) {
        jclass local = (*env)->FindClass(env, "com/casprix/app/A11yNodeProvider");
        if (local) {
            g_provider_cls = (jclass)(*env)->NewGlobalRef(env, local);
            (*env)->DeleteLocalRef(env, local);
        }
    }
    if (g_provider_cls && !g_notify_mid) {
        /* Optional Java hook: static void onNativeContentChanged().
         * If the shim doesn't define it, we simply never post events. */
        g_notify_mid = (*env)->GetStaticMethodID(env, g_provider_cls,
                                                 "onNativeContentChanged", "()V");
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    cpx_a11y_set_content_changed_cb(jni_content_changed);
    cpx_a11y_install_scene_hook();

    __android_log_print(ANDROID_LOG_INFO, CPX_A11Y_TAG,
                        "a11y bridge v1b: surface %dx%d, scene hook installed",
                        (int)w, (int)h);
}

#endif /* __ANDROID__ */
