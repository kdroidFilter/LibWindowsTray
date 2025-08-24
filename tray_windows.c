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
/* Legacy singletons (kept for backward compatibility; no longer used for state) */
static struct tray     *tray_instance  = NULL;   /* unused in multi-instance mode */
static WNDCLASSEX       wc             = {0};
static NOTIFYICONDATAA  nid            = {0};    /* unused in multi-instance mode */
static HWND             hwnd           = NULL;   /* unused in multi-instance mode */
static HMENU            hmenu          = NULL;   /* unused in multi-instance mode */
static UINT             wm_taskbarcreated;
static BOOL             exit_called    = FALSE;  /* unused in multi-instance mode */
static CRITICAL_SECTION tray_cs;
static BOOL             cs_initialized = FALSE;

/* Multi-instance support: one context per tray */
typedef struct TrayContext {
    struct tray *tray;                /* public tray pointer (key)        */
    HWND         hwnd;                /* hidden window for messages       */
    HMENU        hmenu;               /* root menu                        */
    NOTIFYICONDATAA nid;              /* per-icon notify data             */
    UINT         uID;                 /* unique id for Shell_NotifyIcon   */
    DWORD        threadId;            /* thread that owns this context    */
    BOOL         exiting;             /* exit requested for this context  */
    struct TrayContext *next;         /* linked list                      */
} TrayContext;

static TrayContext *g_ctx_head = NULL;
static UINT g_next_uid = ID_TRAY_FIRST;

static void ensure_critical_section(void);
static TrayContext* find_ctx_by_tray(struct tray *t);
static TrayContext* find_ctx_by_hwnd(HWND h);
static TrayContext* find_ctx_by_uid(UINT uid);
static TrayContext* find_ctx_by_thread(DWORD tid);
static TrayContext* create_ctx(struct tray *t);
static void destroy_ctx(TrayContext *ctx);

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
/*  Context helpers                                                            */
/* -------------------------------------------------------------------------- */
static TrayContext* find_ctx_by_tray(struct tray *t)
{
    TrayContext *p = g_ctx_head;
    while (p) {
        if (p->tray == t) return p;
        p = p->next;
    }
    return NULL;
}

static TrayContext* find_ctx_by_hwnd(HWND h)
{
    TrayContext *p = g_ctx_head;
    while (p) {
        if (p->hwnd == h) return p;
        p = p->next;
    }
    return NULL;
}

static TrayContext* find_ctx_by_uid(UINT uid)
{
    TrayContext *p = g_ctx_head;
    while (p) {
        if (p->uID == uid) return p;
        p = p->next;
    }
    return NULL;
}

static TrayContext* find_ctx_by_thread(DWORD tid)
{
    TrayContext *p = g_ctx_head;
    while (p) {
        if (p->threadId == tid) return p;
        p = p->next;
    }
    return NULL;
}

static TrayContext* create_ctx(struct tray *t)
{
    TrayContext *ctx = (TrayContext*)calloc(1, sizeof(TrayContext));
    if (!ctx) return NULL;
    ctx->tray     = t;
    ctx->hwnd     = NULL;
    ctx->hmenu    = NULL;
    ZeroMemory(&ctx->nid, sizeof(ctx->nid));
    ctx->uID      = g_next_uid++;
    ctx->threadId = GetCurrentThreadId();
    ctx->exiting  = FALSE;

    /* Insert at head */
    ctx->next = g_ctx_head;
    g_ctx_head = ctx;
    return ctx;
}

static void destroy_ctx(TrayContext *ctx)
{
    if (!ctx) return;

    /* Remove from list */
    if (g_ctx_head == ctx) {
        g_ctx_head = ctx->next;
    } else {
        TrayContext *prev = g_ctx_head;
        while (prev && prev->next != ctx) prev = prev->next;
        if (prev) prev->next = ctx->next;
    }

    /* Free menu */
    if (ctx->hmenu) {
        MENUITEMINFOA item = {0};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_BITMAP;
        int count = GetMenuItemCount(ctx->hmenu);
        for (int i = 0; i < count; i++) {
            if (GetMenuItemInfoA(ctx->hmenu, i, TRUE, &item)) {
                if (item.hbmpItem && item.hbmpItem != HBMMENU_CALLBACK) {
                    DeleteObject(item.hbmpItem);
                }
            }
        }
        DestroyMenu(ctx->hmenu);
        ctx->hmenu = NULL;
    }

    /* Destroy window */
    if (ctx->hwnd) {
        DestroyWindow(ctx->hwnd);
        ctx->hwnd = NULL;
    }

    /* Free icon handle if any */
    if (ctx->nid.hIcon) {
        DestroyIcon(ctx->nid.hIcon);
        ctx->nid.hIcon = NULL;
    }

    free(ctx);
}

/* ------------------------------------------------------------------ */
/*  Conversion sûre d’une HICON en bitmap ARGB 32 bits                */
/* ------------------------------------------------------------------ */
static HBITMAP bitmap_from_icon(HICON hIcon, int cx, int cy)
{
    if (!hIcon) return NULL;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = cx;
    bi.bmiHeader.biHeight      = -cy;          /* orientation haut‑en‑bas */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;           /* BGRA */
    bi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HDC     hdc  = GetDC(NULL);
    HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbmp) {
        HDC hdcMem   = CreateCompatibleDC(hdc);
        HBITMAP hold = (HBITMAP)SelectObject(hdcMem, hbmp);

        DrawIconEx(hdcMem, 0, 0, hIcon, cx, cy, 0, NULL, DI_NORMAL);

        SelectObject(hdcMem, hold);
        DeleteDC(hdcMem);
    }
    ReleaseDC(NULL, hdc);
    return hbmp;
}

/* ------------------------------------------------------------------ */
/*  Chargement générique d’une icône/bitmap disque → bitmap ARGB      */
/* ------------------------------------------------------------------ */
static HBITMAP load_icon_bitmap(const char *icon_path)
{
    if (!icon_path || !*icon_path) return NULL;

    /* Chemin UTF‑8 → Wide */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, icon_path, -1, NULL, 0);
    if (!wlen) return NULL;

    WCHAR *wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, icon_path, -1, wpath, wlen);

    /* 1º : essai direct .bmp /.png en DIB 32 bits ------------------- */
    HBITMAP hbmp = (HBITMAP)LoadImageW(
        NULL, wpath,
        IMAGE_BITMAP,
        16, 16,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION | LR_DEFAULTSIZE
    );
    if (hbmp) { free(wpath); return hbmp; }

    /* 2º : essai .ico → conversion ARGB ---------------------------- */
    HICON hIcon = (HICON)LoadImageW(
        NULL, wpath,
        IMAGE_ICON,
        16, 16,
        LR_LOADFROMFILE | LR_DEFAULTSIZE
    );
    free(wpath);

    if (hIcon) {
        hbmp = bitmap_from_icon(hIcon, 16, 16);
        DestroyIcon(hIcon);
    }
    return hbmp;          /* NULL si tout a échoué */
}

/* -------------------------------------------------------------------------- */
/*  Invisible window procedure                                                */
/* -------------------------------------------------------------------------- */
static LRESULT CALLBACK tray_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    TrayContext *ctx = find_ctx_by_hwnd(h);
    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_TRAY_CALLBACK_MESSAGE:
        if (l == WM_LBUTTONUP && ctx && ctx->tray && ctx->tray->cb) {
            EnterCriticalSection(&tray_cs);
            if (ctx && ctx->tray && ctx->tray->cb) {
                ctx->tray->cb(ctx->tray);
            }
            LeaveCriticalSection(&tray_cs);
            return 0;
        }
        if (l == WM_LBUTTONUP || l == WM_RBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(h);

            EnterCriticalSection(&tray_cs);
            if (ctx && ctx->hmenu) {
                WORD cmd = TrackPopupMenu(ctx->hmenu,
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
            if (ctx && ctx->hmenu) {
                MENUITEMINFOA item = { 0 };
                item.cbSize = sizeof(item);
                item.fMask = MIIM_ID | MIIM_DATA;
                if (GetMenuItemInfoA(ctx->hmenu, (UINT)w, FALSE, &item)) {
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
            if (ctx) {
                Shell_NotifyIconA(NIM_ADD, &ctx->nid);
            }
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
struct tray *tray_get_instance(void) { 
    ensure_critical_section();
    EnterCriticalSection(&tray_cs);
    TrayContext *ctx = find_ctx_by_thread(GetCurrentThreadId());
    if (!ctx) ctx = g_ctx_head; /* fallback */
    struct tray *t = ctx ? ctx->tray : NULL;
    LeaveCriticalSection(&tray_cs);
    return t;
}

/* -------------------------------------------------------------------------- */
/*  Initializes the tray icon and creates the hidden message window           */
/* -------------------------------------------------------------------------- */
int tray_init(struct tray *tray)
{
    if (!tray) return -1;

    ensure_critical_section();

    tray_enable_dark_mode();
    wm_taskbarcreated = RegisterWindowMessageA("TaskbarCreated");

    // Register (ignore if the class already exists)
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = tray_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = WC_TRAY_CLASS_NAME;
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return -1;

    EnterCriticalSection(&tray_cs);
    TrayContext *ctx = find_ctx_by_tray(tray);
    if (!ctx) ctx = create_ctx(tray);
    LeaveCriticalSection(&tray_cs);
    if (!ctx) return -1;

    ctx->hwnd = CreateWindowExA(
        0,
        WC_TRAY_CLASS_NAME,
        NULL,
        0,
        0, 0, 0, 0,
        0,
        0,
        GetModuleHandleA(NULL),
        NULL);
    if (!ctx->hwnd) return -1;

    ZeroMemory(&ctx->nid, sizeof(ctx->nid));
    ctx->nid.cbSize           = sizeof(ctx->nid);
    ctx->nid.hWnd             = ctx->hwnd;
    ctx->nid.uID              = ctx->uID;
    ctx->nid.uFlags           = NIF_ICON | NIF_MESSAGE;
    ctx->nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
    Shell_NotifyIconA(NIM_ADD, &ctx->nid);

    tray_update(tray);
    return 0;
}

/* Message loop: blocking = 1 -> GetMessage, 0 -> PeekMessage */
int tray_loop(int blocking)
{
    MSG msg;

    /* Ensure there is at least one context for this thread */
    DWORD tid = GetCurrentThreadId();
    EnterCriticalSection(&tray_cs);
    TrayContext *ctx = find_ctx_by_thread(tid);
    LeaveCriticalSection(&tray_cs);
    if (!ctx) return -1;

    if (blocking) {
        int ret = GetMessageA(&msg, NULL, 0, 0);
        if (ret == 0) {
            return -1; /* WM_QUIT */
        } else if (ret < 0) {
            return -1; /* Error */
        }
    } else {
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

    /* Prefer thread-local context to tolerate different struct pointers across updates */
    TrayContext *ctx = find_ctx_by_thread(GetCurrentThreadId());
    if (!ctx) ctx = find_ctx_by_tray(tray);
    if (!ctx) { LeaveCriticalSection(&tray_cs); return; }

    /* Update pointer to reflect latest struct (callbacks, etc.) */
    ctx->tray = tray;

    // Clean up old menu and its bitmaps
    if (ctx->hmenu) {
        MENUITEMINFOA item = {0};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_BITMAP;

        int count = GetMenuItemCount(ctx->hmenu);
        for (int i = 0; i < count; i++) {
            if (GetMenuItemInfoA(ctx->hmenu, i, TRUE, &item)) {
                if (item.hbmpItem && item.hbmpItem != HBMMENU_CALLBACK) {
                    DeleteObject(item.hbmpItem);
                }
            }
        }
        DestroyMenu(ctx->hmenu);
        ctx->hmenu = NULL;
    }

    UINT   id = ID_TRAY_FIRST;
    ctx->hmenu = tray_menu_item(tray->menu, &id);

    /* Icon */
    HICON icon = NULL;
    if (tray->icon_filepath && *tray->icon_filepath) {
        ExtractIconExA(tray->icon_filepath, 0, NULL, &icon, 1);
    }
    if (ctx->nid.hIcon && ctx->nid.hIcon != icon) {
        DestroyIcon(ctx->nid.hIcon);
    }
    ctx->nid.hIcon = icon;

    /* Tooltip */
    ctx->nid.uFlags = NIF_ICON | NIF_MESSAGE;
    if (tray->tooltip && *tray->tooltip) {
        strncpy_s(ctx->nid.szTip, sizeof(ctx->nid.szTip), tray->tooltip, _TRUNCATE);
        ctx->nid.uFlags |= NIF_TIP;
    }

    /* Update the tray */
    Shell_NotifyIconA(NIM_MODIFY, &ctx->nid);

    LeaveCriticalSection(&tray_cs);
}

/* -------------------------------------------------------------------------- */
/*  Cleanly shuts down and unregisters everything                             */
/* -------------------------------------------------------------------------- */
void tray_exit(void)
{
    ensure_critical_section();
    EnterCriticalSection(&tray_cs);

    TrayContext *ctx = find_ctx_by_thread(GetCurrentThreadId());
    if (!ctx) ctx = g_ctx_head; /* fallback */
    if (!ctx) {
        LeaveCriticalSection(&tray_cs);
        return;
    }

    /* Remove tray icon */
    Shell_NotifyIconA(NIM_DELETE, &ctx->nid);
    if (ctx->nid.hIcon) {
        DestroyIcon(ctx->nid.hIcon);
        ctx->nid.hIcon = NULL;
    }

    /* Post WM_QUIT to unblock any blocking GetMessage call and destroy window */
    if (ctx->hwnd) {
        PostMessageA(ctx->hwnd, WM_QUIT, 0, 0);
        DestroyWindow(ctx->hwnd);
        ctx->hwnd = NULL;
    }

    /* Destroy context (frees menu bitmaps, etc.) */
    destroy_ctx(ctx);

    /* If no more contexts, unregister class and optionally release critical section */
    if (!g_ctx_head) {
        UnregisterClassA(WC_TRAY_CLASS_NAME, GetModuleHandleA(NULL));
        LeaveCriticalSection(&tray_cs);
        DeleteCriticalSection(&tray_cs);
        cs_initialized = FALSE;
        return;
    }

    LeaveCriticalSection(&tray_cs);
}

static BOOL get_tray_icon_rect(RECT *r)
{
    /* Use per-thread context to identify the correct tray icon */
    ensure_critical_section();
    EnterCriticalSection(&tray_cs);
    TrayContext *ctx = find_ctx_by_thread(GetCurrentThreadId());
    if (!ctx) ctx = g_ctx_head; /* fallback to first */
    HWND l_hwnd = ctx ? ctx->hwnd : NULL;
    UINT l_uid  = ctx ? ctx->nid.uID : 0;
    LeaveCriticalSection(&tray_cs);

    if (!l_hwnd) return FALSE;

    HMODULE hShell = GetModuleHandleW(L"shell32.dll");
    if (!hShell) return FALSE;

    typedef HRESULT (WINAPI *NIGetRect_t)(const NOTIFYICONIDENTIFIER*, RECT*);
    NIGetRect_t pGetRect = (NIGetRect_t)GetProcAddress(hShell, "Shell_NotifyIconGetRect");
    if (!pGetRect) return FALSE;               /* OS too old */

    NOTIFYICONIDENTIFIER nii = { sizeof(nii) };
    nii.hWnd = l_hwnd;        /* owner window */
    nii.uID  = l_uid;         /* id */

    return SUCCEEDED(pGetRect(&nii, r));
}


/* -------------------------------------------------------------------------- */
/*  Notification area info                                                    */
/* -------------------------------------------------------------------------- */
int tray_get_notification_icons_position(int *x, int *y)    /* <--  BOOL → int */
{
    RECT r = {0};
    BOOL precise = get_tray_icon_rect(&r);   /* TRUE if modern API OK */

    if (precise) {
        /* Use the actual icon rectangle to compute a better anchor */
        int cx = (r.left + r.right) / 2;   /* center X of the icon */
        int cy = 0;                        /* anchor Y: bottom for top tray, top for bottom tray */

        HMONITOR hMon = MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {0};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(hMon, &mi)) {
            LONG midY = (mi.rcMonitor.bottom + mi.rcMonitor.top) / 2;
            /* If icon is on top half of monitor, anchor below it; otherwise above */
            cy = (r.top < midY) ? r.bottom : r.top;
        } else {
            /* Fallback if monitor info is unavailable: use bottom as a reasonable default */
            cy = r.bottom;
        }
        *x = cx;
        *y = cy;
        return 1;                           /* precise */
    } else {
        /* Fallback: use notification area window rect (top-left) */
        HWND hTray  = FindWindowA("Shell_TrayWnd", NULL);
        HWND hNotif = FindWindowExA(hTray, NULL, "TrayNotifyWnd", NULL);
        if (!hNotif || !GetWindowRect(hNotif, &r)) {
            *x = *y = 0;
            return 0;                        /* nothing reliable */
        }
        *x = r.left;
        *y = r.top;
        return 0;                            /* not precise */
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