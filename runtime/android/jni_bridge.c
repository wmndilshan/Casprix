#include "jni_bridge.h"

#ifdef __ANDROID__

#include <android/log.h>
#include <android/native_window_jni.h>

#define CPX_JNI_TAG "CasprixJNI"

static CpxGuiSurfaceAttachFn g_gui_attach_fn = NULL;

void cpx_jni_register_gui_attach(CpxGuiSurfaceAttachFn fn) {
    g_gui_attach_fn = fn;
}

void cpx_jni_clear_gui_attach(void) {
    g_gui_attach_fn = NULL;
}

JNIEXPORT void JNICALL
Java_com_casprix_app_CasprixActivity_nativeWindowCreated(
    JNIEnv* env, jobject obj, jobject surface) {
    ANativeWindow* win;
    int w;
    int h;
    bool attached;

    (void)obj;

    if (!env || !surface) {
        __android_log_print(ANDROID_LOG_WARN, CPX_JNI_TAG,
                            "nativeWindowCreated called without a valid surface");
        return;
    }

    if (!g_gui_attach_fn) {
        __android_log_print(ANDROID_LOG_WARN, CPX_JNI_TAG,
                            "GUI attach callback not registered");
        return;
    }

    win = ANativeWindow_fromSurface(env, surface);
    if (!win) {
        __android_log_print(ANDROID_LOG_ERROR, CPX_JNI_TAG,
                            "ANativeWindow_fromSurface failed");
        return;
    }

    w = ANativeWindow_getWidth(win);
    h = ANativeWindow_getHeight(win);
    attached = g_gui_attach_fn(win, w, h);

    if (!attached) {
        __android_log_print(ANDROID_LOG_ERROR, CPX_JNI_TAG,
                            "GUI surface attach callback returned false (%dx%d)", w, h);
    }
}

#else

void cpx_jni_register_gui_attach(CpxGuiSurfaceAttachFn fn) {
    (void)fn;
}

void cpx_jni_clear_gui_attach(void) {
}

#endif
