#ifndef CPX_ANDROID_JNI_BRIDGE_H
#define CPX_ANDROID_JNI_BRIDGE_H

#include <stdbool.h>

#ifdef __ANDROID__
#include <android/native_window.h>
#include <jni.h>
#else
typedef struct ANativeWindow ANativeWindow;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*CpxGuiSurfaceAttachFn)(ANativeWindow* win, int w, int h);

void cpx_jni_register_gui_attach(CpxGuiSurfaceAttachFn fn);
void cpx_jni_clear_gui_attach(void);

#ifdef __ANDROID__
JNIEXPORT void JNICALL
Java_com_casprix_app_CasprixActivity_nativeWindowCreated(
    JNIEnv* env, jobject obj, jobject surface);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CPX_ANDROID_JNI_BRIDGE_H */
