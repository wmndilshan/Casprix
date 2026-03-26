/*
 * Android Runtime — Activity, Navigation, ViewModel, Intent
 *
 * Provides Android-like application architecture concepts on top of the
 * existing Casperix/Skia scene graph. Works on both:
 *   - Desktop (Windows) via the existing SGApp/SkiaWindow stack
 *   - Android (ARM64) via the NDK ANativeActivity backend
 *
 * Design:
 *   ActivityManager  — screen stack (push/pop navigation)
 *   AndroidActivity  — a named screen with a root SGNode
 *   AndroidIntent    — data bag passed between screens
 *   AndroidViewModel — simple key→value state store per screen
 *   AndroidApp       — top-level application entry point
 */

#ifndef ANDROID_RUNTIME_H
#define ANDROID_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* Pull in scene graph and app types */
#include "scene_graph.h"
#include "frame_loop.h"
#include "animation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Constants
 * ======================================================================== */

#define ANDROID_MAX_ACTIVITIES   16   /* Max navigation stack depth */
#define ANDROID_MAX_INTENT_KEYS  8    /* Max key/value pairs per intent */
#define ANDROID_MAX_VM_SLOTS     32   /* Max ViewModel entries per screen */
#define ANDROID_MAX_STR          256  /* Max general string length */

/* ========================================================================
 * Intent — Data bag passed between screens
 * ======================================================================== */

typedef struct {
    char target[ANDROID_MAX_STR];          /* Target activity name */
    char keys  [ANDROID_MAX_INTENT_KEYS][64];
    char values[ANDROID_MAX_INTENT_KEYS][ANDROID_MAX_STR];
    int  count;                            /* Number of key/value pairs */
} AndroidIntent;

AndroidIntent* android_intent_create(const char* target);
void           android_intent_put   (AndroidIntent* intent, const char* key, const char* value);
const char*    android_intent_get   (AndroidIntent* intent, const char* key);
void           android_intent_destroy(AndroidIntent* intent);

/* ========================================================================
 * ViewModel — Observable per-screen state store
 * ======================================================================== */

typedef struct {
    char   keys  [ANDROID_MAX_VM_SLOTS][64];
    char   values[ANDROID_MAX_VM_SLOTS][ANDROID_MAX_STR];
    int    count;
} AndroidViewModel;

AndroidViewModel* android_viewmodel_create(void);
void              android_viewmodel_destroy(AndroidViewModel* vm);
void              android_viewmodel_set(AndroidViewModel* vm, const char* key, const char* value);
const char*       android_viewmodel_get(AndroidViewModel* vm, const char* key);
void              android_viewmodel_set_int(AndroidViewModel* vm, const char* key, int value);
int               android_viewmodel_get_int(AndroidViewModel* vm, const char* key, int default_val);

/* ========================================================================
 * AndroidActivity — A single screen in the app
 * ======================================================================== */

typedef struct AndroidActivity AndroidActivity;

typedef void (*ActivityOnCreate) (AndroidActivity* activity, AndroidIntent* intent);
typedef void (*ActivityOnResume) (AndroidActivity* activity);
typedef void (*ActivityOnPause)  (AndroidActivity* activity);
typedef void (*ActivityOnDestroy)(AndroidActivity* activity);

struct AndroidActivity {
    char             name[ANDROID_MAX_STR]; /* Unique screen identifier */
    SGNode*          root;                  /* Root scene graph node for this screen */
    AndroidViewModel* viewmodel;            /* Per-screen state (may be NULL) */

    /* Lifecycle callbacks */
    ActivityOnCreate  on_create;
    ActivityOnResume  on_resume;
    ActivityOnPause   on_pause;
    ActivityOnDestroy on_destroy;

    void* user_data;                        /* Arbitrary user pointer */
    int   created;                          /* 1 after on_create fired */
};

AndroidActivity* android_activity_create(const char* name);
void             android_activity_destroy(AndroidActivity* activity);
void             android_activity_set_root(AndroidActivity* activity, SGNode* root);
SGNode*          android_activity_get_root(AndroidActivity* activity);
AndroidViewModel* android_activity_get_viewmodel(AndroidActivity* activity);
const char*      android_activity_get_name(AndroidActivity* activity);

/* ========================================================================
 * ActivityManager — Navigation stack
 * ======================================================================== */

typedef struct {
    AndroidActivity* stack[ANDROID_MAX_ACTIVITIES];
    int              top;            /* Index of current top screen (-1 = empty) */
    SGApp*           app;            /* The underlying Casperix app */
    SGNode*          container;      /* Root node — replaced on each push/pop */
} AndroidActivityManager;

/* Create the activity manager, binding it to an existing SGApp */
AndroidActivityManager* android_activity_manager_create(SGApp* app);
void                    android_activity_manager_destroy(AndroidActivityManager* mgr);

/* Push a new activity onto the stack.
 * Calls on_pause on previous, on_create+on_resume on new. */
void android_push_activity(AndroidActivityManager* mgr,
                            AndroidActivity* activity,
                            AndroidIntent*   intent);

/* Pop the current activity (go back).
 * Calls on_pause+on_destroy on current, on_resume on previous.
 * Returns 0 if stack is empty (no more screens). */
int  android_pop_activity(AndroidActivityManager* mgr);

/* Get the current top activity */
AndroidActivity* android_current_activity(AndroidActivityManager* mgr);

/* Replace current activity without touching history (like startActivity with FLAG_CLEAR_TOP) */
void android_replace_activity(AndroidActivityManager* mgr,
                               AndroidActivity* activity,
                               AndroidIntent*   intent);

/* ========================================================================
 * AndroidApp — Top-level entry point
 * ======================================================================== */

typedef struct {
    SGApp*                  sg_app;   /* Underlying scene graph application */
    AndroidActivityManager* nav;      /* Navigation manager */
    char                    app_name[ANDROID_MAX_STR];
    int                     width, height;
    int                     safe_inset_top;
    int                     safe_inset_right;
    int                     safe_inset_bottom;
    int                     safe_inset_left;
    int                     native_mode;     /* 1 when driven by ANativeActivity */
    void*                   native_assets;   /* AAssetManager* on Android */
    void*                   native_renderer; /* AndroidRenderer* on Android */
} AndroidApp;

/* Create the full Android app */
AndroidApp* android_app_create(const char* app_name, int width, int height);
void        android_app_destroy(AndroidApp* app);

/* Register an activity and push it as the start screen */
void android_app_set_main_activity(AndroidApp* app, AndroidActivity* activity);

/* Run the event loop (blocking) */
void android_app_run(AndroidApp* app);

/* Quit the app */
void android_app_quit(AndroidApp* app);

/* Access helpers */
SGApp*                  android_app_get_sg_app(AndroidApp* app);
AndroidActivityManager* android_app_get_nav(AndroidApp* app);
void                    android_app_bind_current(AndroidApp* app);
void                    android_app_bind_renderer(AndroidApp* app, void* renderer);
void                    android_app_set_safe_insets(AndroidApp* app,
                                                    int left,
                                                    int top,
                                                    int right,
                                                    int bottom);
int                     android_app_get_safe_inset_top(AndroidApp* app);
int                     android_app_get_safe_inset_right(AndroidApp* app);
int                     android_app_get_safe_inset_bottom(AndroidApp* app);
int                     android_app_get_safe_inset_left(AndroidApp* app);

/* ========================================================================
 * Native Android backend API  (called by android_main.c on the render thread)
 * ======================================================================== */

typedef enum {
    ANDROID_LIFECYCLE_RESUME  = 0,
    ANDROID_LIFECYCLE_PAUSE   = 1,
    ANDROID_LIFECYCLE_STOP    = 2,
    ANDROID_LIFECYCLE_DESTROY = 3,
} AndroidLifecycleEvent;

#ifdef __ANDROID__
#  include <android/asset_manager.h>
/* Create app for NativeActivity path: no window spawned, EGL is external */
AndroidApp* android_app_create_native(AAssetManager* assets, int width, int height);
#endif

/* Resize root scene node after a window resize */
void android_app_resize(AndroidApp* app, int width, int height);

/* Advance animations/timers (call once per frame before render) */
void android_app_tick(AndroidApp* app, double current_time_seconds);

/* Begin frame — returns SkCanvas* as void* from the EGL-backed SkSurface */
void* android_app_begin_frame(AndroidApp* app);

/* Draw the current activity root scene into canvas */
void android_app_render(AndroidApp* app, void* canvas);

/* End frame — flush Skia, eglSwapBuffers */
void android_app_end_frame(AndroidApp* app);

/* Deliver a touch/key event into the scene graph hit-test system */
void android_app_dispatch_event(AndroidApp* app, SGEvent* event);

/* Lifecycle state change notification */
void android_app_lifecycle(AndroidApp* app, AndroidLifecycleEvent event);

/* ========================================================================
 * Ripple Effect Animation
 * ======================================================================== */

/* Play a Material Design ripple-style press animation on a node */
void android_ripple_animate(SGAnimationPool* pool, SGNode* node, double duration);


/* ========================================================================
 * Snackbar — Temporary notification overlay
 * ======================================================================== */

typedef struct {
    SGNode*   root;          /* Container node (bottom-anchored row) */
    SGNode*   text_node;     /* Text content */
    int       visible;
    double    dismiss_time;  /* Time to auto-dismiss (seconds from epoch) */
} AndroidSnackbar;

AndroidSnackbar* android_snackbar_create(SGNode* parent_root);
void             android_snackbar_show(AndroidSnackbar* sb, const char* message,
                                        double duration_seconds, void* app_time_ptr);
void             android_snackbar_hide(AndroidSnackbar* sb);
void             android_snackbar_tick(AndroidSnackbar* sb, double current_time);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_RUNTIME_H */
