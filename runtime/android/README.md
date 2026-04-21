# Android-style UI + APK packaging (Casprix)

Skia-based desktop previews and an **APK packaging pipeline** for `.cpx` sources live alongside the main compiler/runtime tree. This folder adds Android-flavored activity/navigation helpers that sit on top of `runtime/skia/` and `lib/android/`.

---

## Folder structure

```
casprix/
├── runtime/
│   ├── skia/               # Skia / Win32 GUI runtime
│   └── android/            # Activity / navigation layer (this directory)
│       ├── android_runtime.h
│       └── android_runtime.c
│
├── lib/
│   ├── skia/               # Low-level Skia .cpx bindings
│   └── android/            # High-level Android framework (.cpx)
│       └── android.cpx
│
├── tools/
│   └── apk_builder/        # APK packaging CLI (CMake target: apk-builder)
│
└── examples/
    └── android/              # Sample apps
```

---

## Quick start (desktop, Windows)

Build with Skia GUI enabled, then run a sample:

```powershell
cmake -S . -B build -DENABLE_SKIA_GUI=ON -DBUILD_RUNTIME=ON -DBUILD_COMPILER=ON
cmake --build build --config Release

.\build\Release\casprix.exe examples\android\hello_android.cpx
```

Paths (`build\Release\` vs `build\`) depend on your CMake generator (Visual Studio vs Ninja).

---

## Build an APK

After configuring the project, build the `apk-builder` tool and run it:

```powershell
cmake --build build --config Release

.\build\Release\apk-builder.exe `
    --input   examples\android\todo_app.cpx `
    --package com.mycompany.todoapp `
    --name    "Todo App" `
    --abi     arm64-v8a `
    --output  todo.apk

adb install todo.apk
```

---

## Framework concepts

### Activity + navigation

```cpx
import "lib/android/android"

let app      = new AndroidApp("My App", 400, 720)
let NAV      = app.nav()

let main_act = android_activity_create("MainActivity")

android_push_activity(NAV, other_activity, 0)
android_pop_activity(NAV)

app.set_main(main_act)
app.run()
```

### Screen + AppBar

```cpx
let screen = new Screen("Home", 0, NAV, font_title, font)
screen.add(my_card.handle)
```

### Material widgets

| Class | Usage |
|-------|--------|
| `MaterialButton` | `.filled()`, `.outlined()`, `.text_btn()`, `.danger()` |
| `MaterialCard` | `.new()`, `.filled(color)`, `.outlined()` |
| `MaterialTextField` | Labeled text input |
| `AppBar` | Top bar with optional back |
| `BottomNavBar` | Up to four tabs |
| `FloatingActionButton` | Circular FAB |
| `ListItem` | Icon + title + subtitle |
| `Chip` | Pill badge |
| `Avatar` | Initials block |
| `Switch` | Toggle |
| `Dialog` | Modal overlay |
| `Snackbar` | Bottom toast |

### ViewModel (state)

```cpx
let vm = new ViewModel()
vm.set("username", "alice")
vm.set_int("count", 42)

let name  = vm.get("username")
let count = vm.get_int("count", 0)
```

### Intent (screen arguments)

```cpx
let intent = new Intent("ProfileScreen")
intent.put("user_id", "42")
intent.put("tab",     "settings")

android_push_activity(NAV, profile_activity, intent.handle)
```

---

## APK builder pipeline

`apk-builder` performs stages such as: compile `.cpx` to native code for the chosen ABI, generate `AndroidManifest.xml`, pack resources, produce an APK archive, sign, and align. Exact steps are implemented in `tools/apk_builder/`.

### Common options

```
apk-builder --input   <file.cpx>
            --package <com.example.app>
            --name    "App Name"
            [--output     app.apk]
            [--version-code 1]
            [--version-name 1.0.0]
            [--min-sdk  24]
            [--target-sdk 34]
            [--abi  arm64-v8a]
            [--ndk  /path/to/ndk]
            [--sdk  /path/to/sdk]
            [--keystore  release.jks]
            [--ks-pass   password]
            [--key-alias mykey]
            [--icon  icon.png]
            [--assets assets/]
            [--fonts  fonts/]
            [--release]
```

### Prerequisites

| Tool | Notes |
|------|--------|
| Android NDK r25+ | Required for ARM native builds |
| `zipalign`, `apksigner` | Android SDK build-tools |
| `adb` | Platform-tools |

On Windows the SDK is often under `%LOCALAPPDATA%\Android\Sdk`. Set `ANDROID_NDK_HOME` if the NDK is not auto-discovered.

---

## Material theme colors (reference)

```cpx
MD_PRIMARY          # 0xFF1976D2
MD_PRIMARY_LIGHT    # 0xFF42A5F5
MD_PRIMARY_DARK     # 0xFF0D47A1
MD_SECONDARY        # 0xFF7B1FA2
MD_SUCCESS          # 0xFF388E3C
MD_ERROR            # 0xFFD32F2F
MD_WARNING          # 0xFFF57C00
MD_SURFACE          # 0xFFFFFFFF
MD_BACKGROUND       # 0xFFF5F5F5
MD_ON_SURFACE       # 0xFF212121
MD_OUTLINE          # 0xFFBDBDBD
```

---

## Examples

| File | Description |
|------|-------------|
| `examples/android/hello_android.cpx` | Minimal starter |
| `examples/android/multi_screen_demo.cpx` | Multi-screen navigation |
| `examples/android/todo_app.cpx` | Todo list with ViewModel |
