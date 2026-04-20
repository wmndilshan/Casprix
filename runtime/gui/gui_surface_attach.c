#include "gui_bridge.h"
#include "support/log.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#if defined(__ANDROID__)
bool cpx_gui_surface_attach(ANativeWindow* window, int w, int h) {
    if (!window || w <= 0 || h <= 0) {
        CPX_LOG(CPX_LOG_WARN, CPX_CAT_RUNTIME, "gui: surface_attach invalid args");
        return false;
    }
    /* Full EGL + Skia hookup lives in runtime/android; this is the stable JNI entry. */
    CPX_LOG(CPX_LOG_INFO, CPX_CAT_RUNTIME, "gui: Android surface attach %dx%d (stub bridge)", w, h);
    (void)window;
    return false;
}
#else
bool cpx_gui_surface_attach(void* window, int w, int h) {
    (void)window;
    (void)w;
    (void)h;
    CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_RUNTIME, "gui: surface_attach called on non-Android platform");
    return false;
}
#endif
