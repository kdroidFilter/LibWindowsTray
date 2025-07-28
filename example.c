/* example.c – Exemple d’utilisation de la bibliothèque tray
   ■ Item "Activer" checkable (coche native ON / icône OFF)
   ■ Sous‑menu « Couleurs » avec icône sur le parent *et* sur chaque entrée */

#include "tray.h"
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/*  Callbacks                                                                */
/* ------------------------------------------------------------------------- */
static void quit_cb(struct tray_menu_item *item) { tray_exit(); }
static void about_cb(struct tray_menu_item *item) { puts("À propos : Exemple de tray icon avec sous‑menu."); }
static void open_cb(struct tray_menu_item *item)  { puts("Ouvrir : action."); }
static void settings_cb(struct tray_menu_item *item) { puts("Paramètres : ouverture."); }

/* Élément checkable : coche native quand ON, icône personnalisée quand OFF */
static void toggle_active_cb(struct tray_menu_item *item) {
    item->checked = !item->checked;
    if (item->checked) {
        item->icon_path = NULL;                                /* coche → ON */
        puts("Activer : ON");
    } else {
        item->icon_path = "C:\\Users\\Elie\\Downloads\\inactive.ico"; /* icône OFF */
        puts("Activer : OFF");
    }
    tray_update(tray_get_instance());                          /* refresh */
}

/* Callback générique pour les items du sous‑menu Couleurs */
static void color_cb(struct tray_menu_item *item) {
    printf("Couleur choisie : %s\n", item->text);
}

static void tray_cb(struct tray *tray) { puts("Clic gauche sur l’icône."); }

/* ------------------------------------------------------------------------- */
/*  Sous‑menu Couleurs : chaque entrée a sa propre icône                     */
/* ------------------------------------------------------------------------- */
static struct tray_menu_item submenu_colors[] = {
    { .text = "Rouge", .cb = color_cb, .disabled = 0, .checked = 0,
      .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico",  .submenu = NULL },
    { .text = "Vert",  .cb = color_cb, .disabled = 0, .checked = 0,
      .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico", .submenu = NULL },
    { .text = "Bleu",  .cb = color_cb, .disabled = 0, .checked = 0,
      .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico",  .submenu = NULL },
    { NULL }
};

/* ------------------------------------------------------------------------- */
/*  Menu principal (terminé par { NULL })                                    */
/* ------------------------------------------------------------------------- */
static struct tray_menu_item tray_menu[] = {
    { .text = "À propos",   .cb = about_cb,         .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    { .text = "Ouvrir",     .cb = open_cb,          .disabled = 0, .checked = 0, .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico", .submenu = NULL },
    { .text = "Activer",    .cb = toggle_active_cb, .disabled = 0, .checked = 1, .icon_path = NULL,                                              .submenu = NULL },
    { .text = "-",          .cb = NULL,             .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    /* Parent du sous‑menu Couleurs avec sa propre icône "palette" */
    { .text = "Couleurs",   .cb = NULL,             .disabled = 0, .checked = 0, .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico", .submenu = submenu_colors },
    { .text = "-",          .cb = NULL,             .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    { .text = "Paramètres", .cb = settings_cb,      .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    { .text = "-",          .cb = NULL,             .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    { .text = "Quitter",    .cb = quit_cb,          .disabled = 0, .checked = 0, .icon_path = NULL,                                              .submenu = NULL },
    { NULL }
};

/* ------------------------------------------------------------------------- */
/*  Main                                                                     */
/* ------------------------------------------------------------------------- */
int main(void) {
    struct tray tray = {
        .icon_filepath = "C:\\Users\\Elie\\Downloads\\favicon.ico",
        .tooltip       = "Exemple Tray Icon",
        .cb            = tray_cb,
        .menu          = tray_menu
    };

    if (tray_init(&tray) < 0) {
        puts("Erreur lors de l’initialisation du tray.");
        return EXIT_FAILURE;
    }

    while (tray_loop(1) == 0) {
        /* Idle – tout est piloté par les callbacks */
    }

    tray_exit();
    return EXIT_SUCCESS;
}
