/* tray.h
 * Public API – remains C99/C++98 compatible
 */
#ifndef TRAY_H
#define TRAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Export                                                                    */
/* -------------------------------------------------------------------------- */
#ifdef _WIN32
#  ifdef TRAY_EXPORTS
#    define TRAY_EXPORT __declspec(dllexport)
#  else
#    define TRAY_EXPORT __declspec(dllimport)
#  endif
#else
#  if __GNUC__ >= 4 || defined(__clang__)
#    define TRAY_EXPORT extern __attribute__((visibility("default")))
#  else
#    define TRAY_EXPORT extern
#  endif
#endif

/* -------------------------------------------------------------------------- */
/*  Structures                                                                */
/* -------------------------------------------------------------------------- */
struct tray_menu_item;

struct tray {
    const char              *icon_filepath;      /* Path to the .ico icon       */
    const char              *tooltip;            /* Tooltip text                */
    void   (*cb)(struct tray *);                 /* Left-click callback (NULL → menu) */
    struct tray_menu_item   *menu;               /* Root menu                   */
};

struct tray_menu_item {
    const char              *text;               /* Label or "-" for separator  */
    int                      disabled;           /* 1 → disabled (greyed out) */
    int                      checked;            /* 1 → checked               */
    void   (*cb)(struct tray_menu_item *);
    struct tray_menu_item   *submenu;            /* NULL if no submenu          */
};

/* -------------------------------------------------------------------------- */
/*  API                                                                       */
/* -------------------------------------------------------------------------- */
TRAY_EXPORT struct tray *tray_get_instance(void);

TRAY_EXPORT int  tray_init (struct tray *tray);
TRAY_EXPORT int  tray_loop (int blocking);       /* 0 = still running, -1 = finished */
TRAY_EXPORT void tray_update(struct tray *tray); /* Refresh menu/info           */
TRAY_EXPORT void tray_exit (void);               /* Free all resources          */

/* Notification area information */
TRAY_EXPORT int tray_get_notification_icons_position(int *x, int *y);
TRAY_EXPORT const char *tray_get_notification_icons_region(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* TRAY_H */ 