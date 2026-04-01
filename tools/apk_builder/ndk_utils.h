/*
 * ndk_utils.h - Android NDK path detection helpers for the APK builder.
 */

#ifndef NDK_UTILS_H
#define NDK_UTILS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detect the Android NDK root directory.
 *
 * Resolution order:
 *   1. ANDROID_NDK_HOME
 *   2. ANDROID_NDK
 *   3. NDK_HOME
 *   4. Platform-specific SDK ndk/ directories under the user's home
 *
 * On success, writes the detected path into out_buf and returns out_buf.
 * On failure, returns NULL and leaves out_buf empty.
 */
const char* cpx_ndk_detect(char* out_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* NDK_UTILS_H */
