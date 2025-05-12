/* tray.c - Windows-only implementation */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <stddef.h>
#include <string.h>
#include "tray.h"

/* -------------------------------------------------------------------------- */
/*  Helpers: opt-in dark mode                                                 */
/* -------------------------------------------------------------------------- */
typedef enum {
    AppMode_Default,
    AppMode_AllowDark,
    AppMode_ForceDark,
    AppMode_ForceLight,
    AppMode_Max
} PreferredAppMode;

static void tray_enable_dark_mode(void)
{
    HMODULE hUx = LoadLibraryW(L"uxtheme.dll");
    if (!hUx) return;

    typedef PreferredAppMode (WINAPI *SetPreferredAppMode_t)(PreferredAppMode);
    SetPreferredAppMode_t SetPreferredAppMode =
        (SetPreferredAppMode_t)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    if (SetPreferredAppMode)
        SetPreferredAppMode(AppMode_AllowDark);
}

/* -------------------------------------------------------------------------- */
/*  Internal constants                                                        */
/* -------------------------------------------------------------------------- */
#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)
#define WC_TRAY_CLASS_NAME       "TRAY"
#define ID_TRAY_FIRST            1000

/* -------------------------------------------------------------------------- */
/*  Internal variables                                                        */
/* -------------------------------------------------------------------------- */
static struct tray     *tray_instance  = NULL;
static WNDCLASSEX       wc             = {0};
static NOTIFYICONDATAA  nid            = {0};
static HWND             hwnd           = NULL;
static HMENU            hmenu          = NULL;
static UINT             wm_taskbarcreated;
static BOOL             exit_called    = FALSE;

/* -------------------------------------------------------------------------- */
/*  Internal prototypes                                                       */
/* -------------------------------------------------------------------------- */
static HMENU tray_menu_item(struct tray_menu_item *m, UINT *id);

/* -------------------------------------------------------------------------- */
/*  Invisible window procedure                                                */
/* -------------------------------------------------------------------------- */
static LRESULT CALLBACK tray_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_TRAY_CALLBACK_MESSAGE:
        if (l == WM_LBUTTONUP && tray_instance && tray_instance->cb) {
            tray_instance->cb(tray_instance);
            return 0;
        }
        if (l == WM_LBUTTONUP || l == WM_RBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(h);
            WORD cmd = TrackPopupMenu(hmenu,
                                      TPM_LEFTALIGN | TPM_RIGHTBUTTON |
                                      TPM_RETURNCMD | TPM_NONOTIFY,
                                      p.x, p.y, 0, h, NULL);
            SendMessage(h, WM_COMMAND, cmd, 0);
            return 0;
        }
        break;

    case WM_COMMAND:
        if (w >= ID_TRAY_FIRST) {
            MENUITEMINFOA item = { .cbSize = sizeof(item), .fMask = MIIM_ID | MIIM_DATA };
            if (GetMenuItemInfoA(hmenu, (UINT)w, FALSE, &item)) {
                struct tray_menu_item *mi = (struct tray_menu_item *)item.dwItemData;
                if (mi && mi->cb) mi->cb(mi);
            }
            return 0;
        }
        break;

    default:
        if (msg == wm_taskbarcreated) {
            Shell_NotifyIconA(NIM_ADD, &nid);
            return 0;
        }
    }
    return DefWindowProcA(h, msg, w, l);
}

/* -------------------------------------------------------------------------- */
/*  Recursive HMENU construction                                              */
/* -------------------------------------------------------------------------- */
static HMENU tray_menu_item(struct tray_menu_item *m, UINT *id)
{
    HMENU menu = CreatePopupMenu();
    for (; m && m->text; ++m, ++(*id)) {
        if (strcmp(m->text, "-") == 0) {
            InsertMenuA(menu, *id, MF_SEPARATOR, TRUE, "");
            continue;
        }

        MENUITEMINFOA info = {0};
        info.cbSize = sizeof(info);
        info.fMask  = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
        info.wID    = *id;
        info.dwTypeData = (LPSTR)m->text;
        info.dwItemData = (ULONG_PTR)m;

        if (m->submenu) {
            info.fMask |= MIIM_SUBMENU;
            info.hSubMenu = tray_menu_item(m->submenu, id);
        }
        if (m->disabled) info.fState |= MFS_DISABLED;
        if (m->checked)  info.fState |= MFS_CHECKED;

        InsertMenuItemA(menu, *id, TRUE, &info);
    }
    return menu;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */
struct tray *tray_get_instance(void) { return tray_instance; }

/* -------------------------------------------------------------------------- */
/*  Initializes the tray icon and creates the hidden message window           */
/* -------------------------------------------------------------------------- */
int tray_init(struct tray *tray)
{
    if (!tray) return -1;

    /* If a previous tray icon is still active, destroy it first */
    if (tray_instance) {
        tray_exit();              // Clean up previous instance
    }
    exit_called = FALSE;          // Allow a fresh lifecycle

    tray_enable_dark_mode();                       // Enable dark mode (optional)
    wm_taskbarcreated = RegisterWindowMessageA("TaskbarCreated");

    // Reset the window class structure to ensure a clean state on each init
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = tray_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = WC_TRAY_CLASS_NAME;

    // Register (ignore if the class already exists)
    if (!RegisterClassExA(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return -1;

    hwnd = CreateWindowExA(
        0,                       // dwExStyle
        WC_TRAY_CLASS_NAME,      // lpClassName
        NULL,                    // lpWindowName
        0,                       // dwStyle
        0, 0, 0, 0,              // X, Y, nWidth, nHeight
        0,                       // hWndParent
        0,                       // hMenu
        GetModuleHandleA(NULL),  // hInstance
        NULL);                   // lpParam

    if (!hwnd) return -1;

    // Setup the notification icon (initially blank)
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = 0;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
    Shell_NotifyIconA(NIM_ADD, &nid);

    tray_update(tray);                              // Populate icon and menu
    return 0;
}

/* Message loop: blocking = 1 -> GetMessage, 0 -> PeekMessage */
int tray_loop(int blocking)
{
    MSG msg;

    if (blocking) {
        /* GetMessage blocks; <= 0  =>  WM_QUIT or error */
        if (GetMessageA(&msg, hwnd, 0, 0) <= 0)
            return -1;
    } else {
        /* PeekMessage doesn't remove? → just continue */
        if (!PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE))
            return 0;
    }

    if (msg.message == WM_QUIT)
        return -1;

    TranslateMessage(&msg);
    DispatchMessageA(&msg);
    return 0;
}

/* Updates icon, tooltip and menu */
void tray_update(struct tray *tray)
{
    if (!tray) return;

    HMENU old = hmenu;
    UINT   id = ID_TRAY_FIRST;
    hmenu = tray_menu_item(tray->menu, &id);

    /* Icon */
    HICON icon = NULL;
    ExtractIconExA(tray->icon_filepath, 0, NULL, &icon, 1);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    nid.hIcon = icon;

    /* Tooltip */
    nid.uFlags = NIF_ICON | NIF_MESSAGE;
    if (tray->tooltip && *tray->tooltip) {
        strncpy_s(nid.szTip, sizeof(nid.szTip), tray->tooltip, _TRUNCATE);
        nid.uFlags |= NIF_TIP;
    }
    Shell_NotifyIconA(NIM_MODIFY, &nid);

    if (old) DestroyMenu(old);
    tray_instance = tray;
}

/* -------------------------------------------------------------------------- */
/*  Cleanly shuts down and unregisters everything                             */
/* -------------------------------------------------------------------------- */
void tray_exit(void)
{
    if (exit_called) return;
    exit_called = TRUE;

    Shell_NotifyIconA(NIM_DELETE, &nid);           // Remove tray icon

    if (nid.hIcon) DestroyIcon(nid.hIcon);
    if (hmenu)     DestroyMenu(hmenu);
    if (hwnd)      DestroyWindow(hwnd);

    UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandleA(NULL));

    // Clear global state for safe reinitialization later
    hwnd  = NULL;
    hmenu = NULL;
    nid   = (NOTIFYICONDATAA){0};
    wc    = (WNDCLASSEXA){0};
    tray_instance = NULL;
}

/* -------------------------------------------------------------------------- */
/*  Notification area info                                                    */
/* -------------------------------------------------------------------------- */
void tray_get_notification_icons_position(int *x, int *y)
{
    RECT r = {0};
    HWND hTray = FindWindowA("Shell_TrayWnd", NULL);
    HWND hNotif = FindWindowExA(hTray, NULL, "TrayNotifyWnd", NULL);

    if (hNotif && GetWindowRect(hNotif, &r)) {
        *x = r.left; *y = r.top;
    } else {
        *x = *y = 0;
    }
}

const char *tray_get_notification_icons_region(void)
{
    RECT  r;
    POINT p = {0, 0};
    HWND  hTray = FindWindowA("Shell_TrayWnd", NULL);
    HWND  hNotif = FindWindowExA(hTray, NULL, "TrayNotifyWnd", NULL);

    if (hNotif && GetWindowRect(hNotif, &r)) {
        p.x = r.left; p.y = r.top;
    }

    HMONITOR hMon = MonitorFromWindow(hNotif, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoA(hMon, &mi);

    LONG midX = (mi.rcMonitor.right  + mi.rcMonitor.left) / 2;
    LONG midY = (mi.rcMonitor.bottom + mi.rcMonitor.top)  / 2;

    if (p.x < midX && p.y < midY) return "top-left";
    if (p.x >= midX && p.y < midY) return "top-right";
    if (p.x < midX && p.y >= midY) return "bottom-left";
    return "bottom-right";
}
