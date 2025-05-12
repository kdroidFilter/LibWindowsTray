# 🖥️ ComposeNativeTray - Windows Tray Backend

This repository contains a minimal, Windows-only C backend for displaying a system tray icon. It is designed for use with the [ComposeNativeTray](https://github.com/kdroidFilter/ComposeNativeTray) library, providing tray integration for Windows applications built with Kotlin and JetBrains Compose.

## 🎯 Purpose

* ✅ Focused only on **Windows** (all other platform code has been removed)
* 🧼 Cleaned up to remove unnecessary files and platform-specific implementations
* ➕ Added support for **tray icon position detection** to help with custom context menu placement
* 🔗 JNI-friendly API for seamless integration with Kotlin/Compose

## ✅ Features

* Add a system tray icon with a tooltip
* Support for left-click callback (optional)
* Customizable context menu:

 * ✔️ Checkable items
 * 🚫 Disabled (grayed-out) items
 * ➕ Submenus
* Dynamic updates of the menu and tooltip at runtime
* **Tray icon screen position detection** for UI alignment

## 🔧 C API

```c
struct tray {
  const char *icon_filepath;
  char *tooltip;
  void (*cb)(struct tray *); // Called on left-click
  struct tray_menu_item *menu; // NULL-terminated array of menu items
};

struct tray_menu_item {
  char *text;
  int disabled;
  int checked;
  void (*cb)(struct tray_menu_item *);
  struct tray_menu_item *submenu; // NULL-terminated submenu
};

// Core API
int tray_init(struct tray *);
void tray_update(struct tray *);
int tray_loop(int blocking);
void tray_exit();
struct tray *tray_get_instance();

// Extra: get the tray icon screen position (custom addition)
bool tray_get_icon_position(POINT *outPosition);
```

All API functions must be called from the UI thread.

## 🔨 Build Instructions

### Requirements

* Visual Studio with CMake support
* CMake 3.15 or later
* Ninja (recommended)

### Build

```sh
mkdir build
cd build
cmake -G Ninja ..
ninja
```

### Demo

Build and run the `tray_example.exe` binary for a working demonstration.

## 📦 JNI Integration

This backend is compiled and linked with `ComposeNativeTray` and accessed from Kotlin using JNI. No external code or platform dependencies are required beyond the Win32 API.

## 🙏 Credits

This fork is based on the great work of:

* [zserge/tray](https://github.com/zserge/tray)
* [StirlingLabs](https://github.com/StirlingLabs/tray)
* Other contributors to related forks and PRs

> This repository is a focused and cleaned-up backend for Windows tray icons. Contributions related to Windows enhancements are welcome. Support for other platforms is out of scope.
