# Android Framework for ND/Casprix Language

A simple, professional Android-like UI framework built on the existing **Skia-based runtime**. Write mobile apps in the ND/Casprix language (`.cpx`) and deploy them as Windows desktop apps or real **Android APKs**.

---

## 📁 Folder Structure

```
ND/
├── runtime/
│   ├── skia/               ← Existing Skia GUI runtime (unchanged)
│   └── android/            ← NEW: Android activity/navigation layer
│       ├── android_runtime.h
│       └── android_runtime.c
│
├── lib/
│   ├── skia/               ← Existing low-level Skia .cpx bindings
│   └── android/            ← NEW: High-level Android framework
│       └── android.cpx     ← Main import point
│
├── tools/
│   └── apk_builder/        ← NEW: APK packaging CLI tool
│       ├── apk_builder.h   ← Public API
│       ├── apk_builder.c   ← 6-stage build pipeline
│       ├── manifest.h/c    ← AndroidManifest.xml generator
│       ├── zip_writer.h/c  ← Minimal ZIP writer (APK = ZIP)
│       └── main.c          ← CLI entry point (apk-builder)
│
└── examples/
    └── android/            ← NEW: Android app examples
        ├── hello_android.cpx       ← Minimal starter
        ├── multi_screen_demo.cpx   ← 3-screen navigation demo
        └── todo_app.cpx            ← Real-world Todo app
```

---

## 🚀 Quick Start

### Run on Desktop (Windows)

```powershell
# Build the compiler + runtime + Skia GUI
cmake -B build -DENABLE_SKIA_GUI=ON -DBUILD_RUNTIME=ON -DBUILD_COMPILER=ON
cmake --build build --config Release

# Run a demo
.\build\Release\casprix.exe examples\android\hello_android.cpx
```

### Build an Android APK

```powershell
# Build the apk-builder tool (included automatically)
cmake --build build --config Release

# Build your .cpx file into a signed APK
.\build\Release\apk-builder.exe `
    --input   examples\android\todo_app.cpx `
    --package com.mycompany.todoapp `
    --name    "Todo App" `
    --abi     arm64-v8a `
    --output  todo.apk

# Install on a connected Android device
adb install todo.apk
```

---

## 📱 Framework Concepts

### Activity + Navigation

```cpx
import "lib/android/android"

let app      = new AndroidApp("My App", 400, 720)
let NAV      = app.nav()

let main_act = android_activity_create("MainActivity")

# Navigate to another screen
android_push_activity(NAV, other_activity, 0)  # push
android_pop_activity(NAV)                       # back

app.set_main(main_act)
app.run()
```

### Screen + AppBar

```cpx
let screen = new Screen("Home", 0, NAV, font_title, font)
screen.add(my_card.handle)
```

### Material Widgets

| Class | Usage |
|---|---|
| `MaterialButton` | `.filled()`, `.outlined()`, `.text_btn()`, `.danger()` |
| `MaterialCard` | `.new()`, `.filled(color)`, `.outlined()` |
| `MaterialTextField` | Labeled text input |
| `AppBar` | Top navigation bar with optional back button |
| `BottomNavBar` | Tab navigation bar (up to 4 tabs) |
| `FloatingActionButton` | Circular primary action button |
| `ListItem` | Icon + title + subtitle row |
| `Chip` | Selectable pill badge |
| `Avatar` | Circular initials block |
| `Switch` | Two-state toggle |
| `Dialog` | Modal card overlay |
| `Snackbar` | Auto-dismiss bottom toast |

### ViewModel (State)

```cpx
let vm = new ViewModel()
vm.set("username", "alice")
vm.set_int("count", 42)

let name  = vm.get("username")
let count = vm.get_int("count", 0)
```

### Intent (Screen Data Passing)

```cpx
let intent = new Intent("ProfileScreen")
intent.put("user_id", "42")
intent.put("tab",     "settings")

android_push_activity(NAV, profile_activity, intent.handle)
```

---

## 🔨 APK Builder Pipeline

`apk-builder` runs 6 stages automatically:

```
1. Compile   .cpx → libMainActivity.so  (ARM64 native, via Casperix compiler)
2. Manifest  Generates AndroidManifest.xml
3. Resources Packs strings.xml, app icon, assets, fonts
4. Package   Bundles into unsigned .apk (ZIP format)
5. Sign      Signs with debug keystore (or your release key)
6. Align     Runs zipalign for Android runtime optimization
```

### Full Options

```
apk-builder --input   <file.cpx>
            --package <com.example.app>
            --name    "App Name"
            [--output     app.apk]
            [--version-code 1]
            [--version-name 1.0.0]
            [--min-sdk  24]
            [--target-sdk 34]
            [--abi  arm64-v8a]        # repeatable
            [--ndk  /path/to/ndk]     # or ANDROID_NDK_HOME env var
            [--sdk  /path/to/sdk]     # or ANDROID_HOME env var
            [--keystore  release.jks]
            [--ks-pass   password]
            [--key-alias mykey]
            [--icon  icon.png]
            [--assets assets/]
            [--fonts  fonts/]
            [--release]               # disables debuggable flag
```

### Prerequisites

| Tool | Where to get |
|---|---|
| Android NDK r25+ | [developer.android.com/ndk](https://developer.android.com/ndk) |
| `zipalign` | Android SDK Build Tools (included with Android Studio) |
| `apksigner` | Android SDK Build Tools (included with Android Studio) |
| `adb` | Android SDK Platform Tools — **already installed** ✓ |

**SDK auto-detected** from `%LOCALAPPDATA%\Android\Sdk` (the default Android Studio install path).  
Only the NDK path needs to be set manually if not installed alongside the SDK:

```powershell
# Only required if NDK is not inside SDK\ndk\<version>\
$env:ANDROID_NDK_HOME = "C:\Users\User\AppData\Local\Android\Sdk\ndk\25.2.9519653"
```

If the NDK *is* inside the SDK folder, `apk-builder` finds it automatically.

---

## 🎨 Material Theme Colors

```cpx
MD_PRIMARY          # 0xFF1976D2  Blue 700
MD_PRIMARY_LIGHT    # 0xFF42A5F5  Blue 400
MD_PRIMARY_DARK     # 0xFF0D47A1  Blue 900
MD_SECONDARY        # 0xFF7B1FA2  Purple 700
MD_SUCCESS          # 0xFF388E3C  Green 700
MD_ERROR            # 0xFFD32F2F  Red 700
MD_WARNING          # 0xFFF57C00  Orange 700
MD_SURFACE          # 0xFFFFFFFF  White
MD_BACKGROUND       # 0xFFF5F5F5  Grey 100
MD_ON_SURFACE       # 0xFF212121  Grey 900
MD_OUTLINE          # 0xFFBDBDBD  Grey 400
```

---

## 📖 Examples

| Example | Description |
|---|---|
| `hello_android.cpx` | Minimal starter — one screen with a card and button |
| `multi_screen_demo.cpx` | Login → Dashboard → Profile with full navigation |
| `todo_app.cpx` | Real-world todo list with ViewModel state |
