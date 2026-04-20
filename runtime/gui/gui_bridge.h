#ifndef CASPRIX_GUI_BRIDGE_H
#define CASPRIX_GUI_BRIDGE_H

#include <stdbool.h>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ANDROID__)
bool cpx_gui_surface_attach(ANativeWindow* window, int w, int h);
#else
bool cpx_gui_surface_attach(void* window, int w, int h);
#endif

void cpx_gui_training_progress(float loss, int step);

#ifdef __cplusplus
}
#endif
#endif
