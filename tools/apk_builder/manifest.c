/*
 * manifest.c — AndroidManifest.xml generator for Casperix APK Builder
 */

#include "manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define MKDIR(p) mkdir(p, 0755)
#endif

static void manifest_copy_str(char* dst, size_t dst_size, const char* src, const char* fallback) {
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", (src && src[0]) ? src : (fallback ? fallback : ""));
}

static int manifest_is_empty(const char* s) {
    return !s || !s[0];
}

/* Repo-relative location of the accessibility-shim Java sources. */
#define CPX_A11Y_JAVA_DIR "runtime/android/java"

int apk_accessibility_shim_enabled(const ApkBuildConfig* config,
                                   char* java_src_dir, size_t java_src_dir_size) {
    char probe[1024];
    FILE* f;
    int have_sources;

    snprintf(probe, sizeof(probe),
             "%s/com/casprix/app/CasprixNativeActivity.java", CPX_A11Y_JAVA_DIR);
    f = fopen(probe, "rb");
    have_sources = (f != NULL);
    if (f) fclose(f);

    if (java_src_dir && java_src_dir_size) {
        snprintf(java_src_dir, java_src_dir_size, "%s", CPX_A11Y_JAVA_DIR);
    }

    if (!config) return have_sources;
    if (config->accessibility_shim == 0) return 0;   /* forced off */
    if (config->accessibility_shim > 0) return 1;    /* forced on  */
    return have_sources;                             /* auto       */
}

void manifest_builder_init(ManifestBuilder* mb, const ApkBuildConfig* config) {
    if (!mb) return;
    memset(mb, 0, sizeof(*mb));
    mb->config = config;

    if (config) {
        manifest_copy_str(mb->package_name, sizeof(mb->package_name), config->package_name, "");
        manifest_copy_str(mb->app_name, sizeof(mb->app_name), config->app_name, "");
        manifest_copy_str(mb->main_activity, sizeof(mb->main_activity), config->main_activity, "MainActivity");
        mb->min_sdk = config->min_sdk ? config->min_sdk : 24;
        mb->target_sdk = config->target_sdk ? config->target_sdk : 34;
        mb->version_code = config->version_code ? config->version_code : 1;
        manifest_copy_str(mb->version_name, sizeof(mb->version_name), config->version_name, "1.0.0");
        mb->debuggable = config->debug_build ? 1 : 0;
    } else {
        manifest_copy_str(mb->main_activity, sizeof(mb->main_activity), "MainActivity", "");
        manifest_copy_str(mb->version_name, sizeof(mb->version_name), "1.0.0", "");
        mb->min_sdk = 24;
        mb->target_sdk = 34;
        mb->version_code = 1;
    }

    manifest_add_feature(mb, "android.hardware.opengles.a2", 1);
    if (mb->feature_count > 0) {
        snprintf(mb->features[0].gl_es_version, sizeof(mb->features[0].gl_es_version), "0x00020000");
    }
}

int manifest_add_permission(ManifestBuilder* mb, const char* permission_name) {
    if (!mb || manifest_is_empty(permission_name)) return -1;
    if (mb->permission_count >= MANIFEST_MAX_PERMISSIONS) return -1;
    mb->permissions[mb->permission_count++] = permission_name;
    return 0;
}

int manifest_add_feature(ManifestBuilder* mb, const char* feature_name, int required) {
    if (!mb || manifest_is_empty(feature_name)) return -1;
    if (mb->feature_count >= MANIFEST_MAX_FEATURES) return -1;

    ManifestFeature* feature = &mb->features[mb->feature_count++];
    memset(feature, 0, sizeof(*feature));
    snprintf(feature->name, sizeof(feature->name), "%s", feature_name);
    feature->required = required ? 1 : 0;
    return 0;
}

int manifest_add_activity(ManifestBuilder* mb, const char* activity_name,
                          ManifestActivityStyle style, const char* label,
                          const char* lib_name) {
    if (!mb || manifest_is_empty(activity_name)) return -1;
    if (mb->activity_count >= MANIFEST_MAX_ACTIVITIES) return -1;

    ManifestActivity* activity = &mb->activities[mb->activity_count++];
    memset(activity, 0, sizeof(*activity));
    snprintf(activity->name, sizeof(activity->name), "%s", activity_name);
    manifest_copy_str(activity->label, sizeof(activity->label), label, "");
    manifest_copy_str(activity->lib_name, sizeof(activity->lib_name), lib_name, "");
    snprintf(activity->config_changes, sizeof(activity->config_changes),
             "orientation|keyboardHidden|screenSize");
    snprintf(activity->screen_orientation, sizeof(activity->screen_orientation), "%s", "portrait");
    snprintf(activity->theme, sizeof(activity->theme),
             "@android:style/Theme.DeviceDefault.NoActionBar");
    snprintf(activity->window_soft_input_mode, sizeof(activity->window_soft_input_mode),
             "adjustResize");
    activity->exported = (mb->activity_count == 1) ? 1 : 0;
    activity->launchable = (mb->activity_count == 1) ? 1 : 0;
    activity->style = style;
    return 0;
}

static void manifest_write_feature(FILE* f, const ManifestFeature* feature) {
    if (!feature) return;
    if (feature->gl_es_version[0]) {
        fprintf(f,
            "    <uses-feature android:glEsVersion=\"%s\"\n"
            "        android:required=\"%s\" />\n",
            feature->gl_es_version,
            feature->required ? "true" : "false");
        return;
    }

    fprintf(f,
        "    <uses-feature android:name=\"%s\"\n"
        "        android:required=\"%s\" />\n",
        feature->name,
        feature->required ? "true" : "false");
}

static void manifest_write_activity(FILE* f, const ManifestActivity* activity) {
    if (!activity) return;

    fprintf(f, "        <activity\n");
    fprintf(f, "            android:name=\"%s\"\n", activity->name);
    if (activity->label[0]) {
        fprintf(f, "            android:label=\"%s\"\n", activity->label);
    }
    if (activity->config_changes[0]) {
        fprintf(f, "            android:configChanges=\"%s\"\n", activity->config_changes);
    }
    if (activity->screen_orientation[0]) {
        fprintf(f, "            android:screenOrientation=\"%s\"\n", activity->screen_orientation);
    }
    if (activity->theme[0]) {
        fprintf(f, "            android:theme=\"%s\"\n", activity->theme);
    }
    if (activity->window_soft_input_mode[0]) {
        fprintf(f, "            android:windowSoftInputMode=\"%s\"\n", activity->window_soft_input_mode);
    }
    if (activity->style == MANIFEST_ACTIVITY_SINGLE_TOP) {
        fprintf(f, "            android:launchMode=\"singleTop\"\n");
    }
    fprintf(f, "            android:exported=\"%s\">\n", activity->exported ? "true" : "false");

    if (activity->style == MANIFEST_ACTIVITY_NATIVE) {
        fprintf(f,
            "\n"
            "            <!-- Name of the shared library without 'lib' prefix and '.so' suffix -->\n"
            "            <meta-data\n"
            "                android:name=\"android.app.lib_name\"\n"
            "                android:value=\"%s\" />\n",
            activity->lib_name[0] ? activity->lib_name : activity->name);
    }

    if (activity->launchable) {
        fprintf(f,
            "\n"
            "            <intent-filter>\n"
            "                <action android:name=\"android.intent.action.MAIN\" />\n"
            "                <category android:name=\"android.intent.category.LAUNCHER\" />\n"
            "            </intent-filter>\n");
    }

    fprintf(f, "        </activity>\n");
}

int manifest_write_builder(const ManifestBuilder* mb, const char* output_path) {
    if (!mb || !output_path) return -1;

    FILE* f = fopen(output_path, "w");
    if (!f) return -1;

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"%s\"\n"
        "    android:versionCode=\"%d\"\n"
        "    android:versionName=\"%s\">\n"
        "\n"
        "    <uses-sdk\n"
        "        android:minSdkVersion=\"%d\"\n"
        "        android:targetSdkVersion=\"%d\" />\n"
        "\n",
        mb->package_name,
        mb->version_code,
        mb->version_name,
        mb->min_sdk,
        mb->target_sdk);

    for (int i = 0; i < mb->permission_count; i++) {
        fprintf(f, "    <uses-permission android:name=\"%s\" />\n", mb->permissions[i]);
    }

    if (mb->permission_count > 0) {
        fprintf(f, "\n");
    }

    for (int i = 0; i < mb->feature_count; i++) {
        manifest_write_feature(f, &mb->features[i]);
    }

    fprintf(f,
        "\n"
        "    <application\n"
        "        android:label=\"@string/app_name\"\n"
        "        android:icon=\"@drawable/ic_launcher\"\n"
        "        android:hasCode=\"%s\"\n"
        "        android:debuggable=\"%s\"\n"
        "        android:allowBackup=\"true\"\n"
        "        android:hardwareAccelerated=\"true\">\n"
        "\n",
        mb->has_code ? "true" : "false",
        mb->debuggable ? "true" : "false");

    for (int i = 0; i < mb->activity_count; i++) {
        manifest_write_activity(f, &mb->activities[i]);
    }

    fprintf(f, "    </application>\n</manifest>\n");
    fclose(f);
    return 0;
}

int manifest_write(const ApkBuildConfig* config, const char* output_path) {
    ManifestBuilder mb;
    manifest_builder_init(&mb, config);

    const char* lib_name = mb.main_activity[0] ? mb.main_activity : "MainActivity";

    /* Accessibility v1a: when the shim is enabled, the entry point becomes the
     * Java NativeActivity subclass com.casprix.app.CasprixNativeActivity. It
     * is still MANIFEST_ACTIVITY_NATIVE (keeps the android.app.lib_name
     * meta-data pointing at lib<name>.so, so the native entry path is
     * completely unchanged), but the class name is Java and hasCode=true. */
    int shim = config &&
               apk_accessibility_shim_enabled(config, NULL, 0);
    if (shim) {
        mb.has_code = 1;
    }

    if (mb.activity_count == 0) {
        manifest_add_activity(&mb,
                              shim ? "com.casprix.app.CasprixNativeActivity"
                                   : lib_name,
                              MANIFEST_ACTIVITY_NATIVE,
                              mb.app_name,
                              lib_name);
    }
    return manifest_write_builder(&mb, output_path);
}

int manifest_write_strings(const ApkBuildConfig* config, const char* res_dir) {
    /* Create res/values/ directory */
    char values_dir[768];
    snprintf(values_dir, sizeof(values_dir), "%s/values", res_dir);
    MKDIR(values_dir);

    char path[1024];
    snprintf(path, sizeof(path), "%s/strings.xml", values_dir);

    FILE* f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<resources>\n"
        "    <string name=\"app_name\">%s</string>\n"
        "</resources>\n",
        config->app_name);

    fclose(f);
    return 0;
}

int manifest_write_default_icon(const char* drawable_dir) {
    MKDIR(drawable_dir);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ic_launcher.xml", drawable_dir);

    FILE* f = fopen(path, "w");
    if (!f) return -1;

    /* Minimal adaptive icon vector */
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<vector xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    android:width=\"108dp\"\n"
        "    android:height=\"108dp\"\n"
        "    android:viewportWidth=\"108\"\n"
        "    android:viewportHeight=\"108\">\n"
        "  <path android:fillColor=\"#1976D2\"\n"
        "      android:pathData=\"M0,0h108v108h-108z\" />\n"
        "  <path android:fillColor=\"#FFFFFF\"\n"
        "      android:pathData=\"M34,54 L54,34 L74,54 L54,74Z\" />\n"
        "</vector>\n");

    fclose(f);
    return 0;
}
