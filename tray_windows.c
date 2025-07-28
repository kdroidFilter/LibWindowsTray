/* tray.c - Windows-only implementation with icon support */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <stddef.h>
#include <stdlib.h>
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
static CRITICAL_SECTION tray_cs;
static BOOL             cs_initialized = FALSE;

/* -------------------------------------------------------------------------- */
/*  Internal prototypes                                                       */
/* -------------------------------------------------------------------------- */
static HMENU tray_menu_item(struct tray_menu_item *m, UINT *id);
static void ensure_critical_section(void);
static HBITMAP load_icon_bitmap(const char *icon_path);

/* -------------------------------------------------------------------------- */
/*  Critical section helper                                                   */
/* -------------------------------------------------------------------------- */
static void ensure_critical_section(void)
{
    if (!cs_initialized) {
        InitializeCriticalSection(&tray_cs);
        cs_initialized = TRUE;
    }
}

/* -------------------------------------------------------------------------- */
/*  Load icon as bitmap for menu items                                       */
/* -------------------------------------------------------------------------- */
static HBITMAP load_icon_bitmap(const char *icon_path)
{
    if (!icon_path || !*icon_path) return NULL;

    // Convert char* to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, icon_path, -1, NULL, 0);
    if (wlen == 0) return NULL;

    WCHAR *wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!wpath) return NULL;

    MultiByteToWideChar(CP_UTF8, 0, icon_path, -1, wpath, wlen);

    // Load image as bitmap
    HBITMAP hBitmap = (HBITMAP)LoadImageW(
        NULL,
        wpath,
        IMAGE_BITMAP,
        16,  // Standard menu icon width
        16,  // Standard menu icon height
        LR_LOADFROMFILE | LR_LOADTRANSPARENT
    );

    // If loading as bitmap fails, try loading as icon and convert
    if (!hBitmap) {
        HICON hIcon = (HICON)LoadImageW(
            NULL,
            wpath,
            IMAGE_ICON,
            16,
            16,
            LR_LOADFROMFILE
        );

        if (hIcon) {
            // Convert icon to bitmap
            HDC hDC = GetDC(NULL);
            if (!hDC) {
                DestroyIcon(hIcon);
                free(wpath);
                return NULL;
            }

            HDC hMemDC = CreateCompatibleDC(hDC);
            if (!hMemDC) {
                ReleaseDC(NULL, hDC);
                DestroyIcon(hIcon);
                free(wpath);
                return NULL;
            }

            hBitmap = CreateCompatibleBitmap(hDC, 16, 16);
            if (!hBitmap) {
                DeleteDC(hMemDC);
                ReleaseDC(NULL, hDC);
                DestroyIcon(hIcon);
                free(wpath);
                return NULL;
            }

            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
            if (hOldBitmap) {
                // Succeeded in selecting
                RECT rc = {0, 0, 16, 16};
                FillRect(hMemDC, &rc, GetSysColorBrush(COLOR_MENU));  // Use menu background color for better blending

                DrawIconEx(hMemDC, 0, 0, hIcon, 16, 16, 0, NULL, DI_NORMAL);

                SelectObject(hMemDC, hOldBitmap);
            } else {
                // Select failed; discard bitmap
                DeleteObject(hBitmap);
                hBitmap = NULL;
            }

            DeleteDC(hMemDC);
            ReleaseDC(NULL, hDC);
            DestroyIcon(hIcon);
        }
    }

    free(wpath);
    return hBitmap;
}
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
            EnterCriticalSection(&tray_cs);
            if (tray_instance && tray_instance->cb) {
                tray_instance->cb(tray_instance);
            }
            LeaveCriticalSection(&tray_cs);
            return 0;
        }
        if (l == WM_LBUTTONUP || l == WM_RBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(h);

            EnterCriticalSection(&tray_cs);
            if (hmenu) {
                WORD cmd = TrackPopupMenu(hmenu,
                                          TPM_LEFTALIGN | TPM_RIGHTBUTTON |
                                          TPM_RETURNCMD | TPM_NONOTIFY,
                                          p.x, p.y, 0, h, NULL);
                SendMessage(h, WM_COMMAND, cmd, 0);
            }
            LeaveCriticalSection(&tray_cs);
            return 0;
        }
        break;

    case WM_COMMAND:
        if (w >= ID_TRAY_FIRST) {
            EnterCriticalSection(&tray_cs);
            if (hmenu) {
                MENUITEMINFOA item = { .cbSize = sizeof(item), .fMask = MIIM_ID | MIIM_DATA };
                if (GetMenuItemInfoA(hmenu, (UINT)w, FALSE, &item)) {
                    struct tray_menu_item *mi = (struct tray_menu_item *)item.dwItemData;
                    if (mi && mi->cb) mi->cb(mi);
                }
            }
            LeaveCriticalSection(&tray_cs);
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
/*  Recursive HMENU construction with safe icon support                       */
/* -------------------------------------------------------------------------- */
static HMENU tray_menu_item(struct tray_menu_item *m, UINT *id)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;

    for (; m && m->text; ++m) {

        /* ------------------------------------------------------------------ */
        /*  Séparateur « - »                                                 */
        /* ------------------------------------------------------------------ */
        if (strcmp(m->text, "-") == 0) {
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            continue;
        }

        /* ------------------------------------------------------------------ */
        /*  Élément normal (texte + éventuelle icône + sous‑menu)             */
        /* ------------------------------------------------------------------ */
        MENUITEMINFOA info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);

        /* Texte : MIIM_STRING + MFT_STRING au lieu de MIIM_TYPE              */
        info.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE |
                          MIIM_FTYPE | MIIM_DATA;
        info.fType      = MFT_STRING;
        info.dwTypeData = (LPSTR)m->text;
        info.cch        = (UINT)strlen(m->text);

        /* Identifiant unique                                                */
        info.wID        = (*id)++;                 /* consomme un ID          */
        info.dwItemData = (ULONG_PTR)m;            /* pointeur item → cb      */

        /* Sous‑menu éventuel                                                */
        if (m->submenu) {
            info.fMask   |= MIIM_SUBMENU;
            info.hSubMenu = tray_menu_item(m->submenu, id);
        }

        /* État (désactivé / coché)                                          */
        if (m->disabled) info.fState |= MFS_DISABLED;
        if (m->checked)  info.fState |= MFS_CHECKED;

        /* Icône optionnelle                                                 */
        if (m->icon_path && *m->icon_path) {
            HBITMAP hBmp = load_icon_bitmap(m->icon_path);
            if (hBmp) {
                info.fMask    |= MIIM_BITMAP;      /*  OK avec MIIM_STRING    */
                info.hbmpItem  = hBmp;
            }
        }

        /* Append en fin de menu pour éviter les index hors‑plage            */
        InsertMenuItemA(menu, (UINT)-1, TRUE, &info);
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

    ensure_critical_section();

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

    if (!hwnd) return -1;  // Ensure hwnd is valid

    if (blocking) {
        /* GetMessage blocks; <= 0  =>  WM_QUIT or error */
        /* IMPORTANT: Use NULL instead of hwnd to get all thread messages */
        int ret = GetMessageA(&msg, NULL, 0, 0);
        if (ret == 0) {
            /* WM_QUIT received */
            return -1;
        } else if (ret < 0) {
            /* Error */
            return -1;
        }
    } else {
        /* PeekMessage doesn't remove? → just continue */
        /* IMPORTANT: Use NULL instead of hwnd to get all thread messages */
        if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
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

    ensure_critical_section();
    EnterCriticalSection(&tray_cs);

    // Clean up old menu and its bitmaps
    if (hmenu) {
        // Recursively clean up bitmaps from menu items
        MENUITEMINFOA item = {0};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_BITMAP;

        int count = GetMenuItemCount(hmenu);
        for (int i = 0; i < count; i++) {
            if (GetMenuItemInfoA(hmenu, i, TRUE, &item)) {
                if (item.hbmpItem && item.hbmpItem != HBMMENU_CALLBACK) {
                    DeleteObject(item.hbmpItem);
                }
            }
        }
        DestroyMenu(hmenu);
    }

    UINT   id = ID_TRAY_FIRST;
    hmenu = tray_menu_item(tray->menu, &id);

    /* Icon */
    HICON icon = NULL;
    if (tray->icon_filepath && *tray->icon_filepath) {
        ExtractIconExA(tray->icon_filepath, 0, NULL, &icon, 1);
    }
    if (nid.hIcon && nid.hIcon != icon) {
        DestroyIcon(nid.hIcon);
    }
    nid.hIcon = icon;

    /* Tooltip */
    nid.uFlags = NIF_ICON | NIF_MESSAGE;
    if (tray->tooltip && *tray->tooltip) {
        strncpy_s(nid.szTip, sizeof(nid.szTip), tray->tooltip, _TRUNCATE);
        nid.uFlags |= NIF_TIP;
    }

    /* Store instance before modifying */
    tray_instance = tray;

    /* Update the tray */
    Shell_NotifyIconA(NIM_MODIFY, &nid);

    LeaveCriticalSection(&tray_cs);
}

/* -------------------------------------------------------------------------- */
/*  Cleanly shuts down and unregisters everything                             */
/* -------------------------------------------------------------------------- */
void tray_exit(void)
{
    if (exit_called) return;
    exit_called = TRUE;

    if (cs_initialized) {
        EnterCriticalSection(&tray_cs);
    }

    Shell_NotifyIconA(NIM_DELETE, &nid);           // Remove tray icon

    if (nid.hIcon) DestroyIcon(nid.hIcon);

    // Clean up menu and its bitmaps
    if (hmenu) {
        MENUITEMINFOA item = {0};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_BITMAP;

        int count = GetMenuItemCount(hmenu);
        for (int i = 0; i < count; i++) {
            if (GetMenuItemInfoA(hmenu, i, TRUE, &item)) {
                if (item.hbmpItem && item.hbmpItem != HBMMENU_CALLBACK) {
                    DeleteObject(item.hbmpItem);
                }
            }
        }
        DestroyMenu(hmenu);
    }

    /* Post WM_QUIT to unblock any blocking GetMessage call */
    if (hwnd) {
        PostMessageA(hwnd, WM_QUIT, 0, 0);
        DestroyWindow(hwnd);
    }

    UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandleA(NULL));

    // Clear global state for safe reinitialization later
    hwnd  = NULL;
    hmenu = NULL;
    nid   = (NOTIFYICONDATAA){0};
    wc    = (WNDCLASSEXA){0};
    tray_instance = NULL;

    if (cs_initialized) {
        LeaveCriticalSection(&tray_cs);
        DeleteCriticalSection(&tray_cs);
        cs_initialized = FALSE;
    }
}

static BOOL get_tray_icon_rect(RECT *r)
{
    HMODULE hShell = GetModuleHandleW(L"shell32.dll");
    if (!hShell) return FALSE;

    typedef HRESULT (WINAPI *NIGetRect_t)(const NOTIFYICONIDENTIFIER*, RECT*);
    NIGetRect_t pGetRect = (NIGetRect_t)GetProcAddress(hShell, "Shell_NotifyIconGetRect");
    if (!pGetRect) return FALSE;               /* OS trop ancien */

    NOTIFYICONIDENTIFIER nii = { sizeof(nii) };
    nii.hWnd = hwnd;        /* fenêtre propriétaire */
    nii.uID  = nid.uID;     /* identifiant (0 ici) */
    /*  sii.guidItem = nid.guidItem;  // si tu en utilises un */

    return SUCCEEDED(pGetRect(&nii, r));
}


/* -------------------------------------------------------------------------- */
/*  Notification area info                                                    */
/* -------------------------------------------------------------------------- */
int tray_get_notification_icons_position(int *x, int *y)    /* <--  BOOL → int */
{
    RECT r = {0};
    BOOL precise = get_tray_icon_rect(&r);   /* TRUE si API moderne OK */

    if (!precise) {                          /* → on passe en fallback */
        HWND hTray  = FindWindowA("Shell_TrayWnd", NULL);
        HWND hNotif = FindWindowExA(hTray, NULL, "TrayNotifyWnd", NULL);
        if (!hNotif || !GetWindowRect(hNotif, &r)) {
            *x = *y = 0;
            return 0;                        /* rien de fiable : abort */
        }
    }

    *x = r.left;
    *y = r.top;
    return precise ? 1 : 0;                  /* 1 = précis, 0 = fallback */
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