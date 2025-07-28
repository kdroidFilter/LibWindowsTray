/* example.c - Exemple simple d'utilisation de la bibliothèque tray pour Windows avec icônes sur les items de menu */

#include "tray.h"
#include <stdio.h>
#include <stdlib.h>

// Fonction de rappel pour un élément de menu (par exemple, "Quitter")
static void quit_cb(struct tray_menu_item *item) {
    tray_exit();
}

// Fonction de rappel pour un élément de menu (par exemple, "À propos")
static void about_cb(struct tray_menu_item *item) {
    printf("À propos : Ceci est un exemple de tray icon.\n");
}

// Fonction de rappel pour un élément de menu (par exemple, "Ouvrir")
static void open_cb(struct tray_menu_item *item) {
    printf("Ouvrir : Action d'ouverture.\n");
}

// Fonction de rappel pour un élément de menu (par exemple, "Paramètres")
static void settings_cb(struct tray_menu_item *item) {
    printf("Paramètres : Ouverture des paramètres.\n");
}

// Fonction de rappel pour le clic gauche sur l'icône (optionnel)
static void tray_cb(struct tray *tray) {
    printf("Clic gauche sur l'icône du tray.\n");
}

// Définition du menu (terminé par { NULL }), avec des icônes sur certains items
static struct tray_menu_item tray_menu[] = {
    { .text = "À propos", .cb = about_cb, .disabled = 0, .checked = 0, .icon_path = NULL, .submenu = NULL },  // Sans icône pour tester
    { .text = "Ouvrir", .cb = open_cb, .disabled = 0, .checked = 0, .icon_path = "C:\\Users\\Elie\\Downloads\\favicon.ico", .submenu = NULL },
    { .text = "-", .cb = NULL, .disabled = 0, .checked = 0, .icon_path = NULL, .submenu = NULL },               // Séparateur
    { .text = "Paramètres", .cb = settings_cb, .disabled = 0, .checked = 0, .icon_path = NULL, .submenu = NULL },
    { .text = "-", .cb = NULL, .disabled = 0, .checked = 0, .icon_path = NULL, .submenu = NULL },               // Séparateur
    { .text = "Quitter", .cb = quit_cb, .disabled = 0, .checked = 0, .icon_path = NULL, .submenu = NULL },
    { NULL }  // Fin du menu
};

int main() {
    struct tray tray = {
        .icon_filepath = "C:\\Users\\Elie\\Downloads\\favicon.ico",
        .tooltip = "Exemple Tray Icon",
        .cb = tray_cb,
        .menu = tray_menu
    };

    if (tray_init(&tray) < 0) {
        printf("Erreur lors de l'initialisation du tray.\n");
        return EXIT_FAILURE;
    }

    while (tray_loop(1) == 0) {
        // Boucle principale
    }

    tray_exit();
    return EXIT_SUCCESS;
}