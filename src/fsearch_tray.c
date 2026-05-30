/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define G_LOG_DOMAIN "fsearch-tray"

#include "fsearch_tray.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#include <gtk/gtk.h>
#include <glib/gi18n.h>

struct FsearchTray {
#ifdef HAVE_AYATANA_APPINDICATOR
    AppIndicator *indicator;
#endif
    FsearchApplication *app;
};

#ifdef HAVE_AYATANA_APPINDICATOR
static void
on_tray_search_activated(GtkMenuItem *item, gpointer user_data) {
    FsearchApplication *app = user_data;
    g_application_activate(G_APPLICATION(app));
}

static void
on_tray_rescan_activated(GtkMenuItem *item, gpointer user_data) {
    FsearchApplication *app = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(app), "update_database", NULL);
}

static void
on_tray_quit_activated(GtkMenuItem *item, gpointer user_data) {
    FsearchApplication *app = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
}
#endif

FsearchTray *
fsearch_tray_new(FsearchApplication *app) {
    FsearchTray *tray = g_new0(FsearchTray, 1);
    tray->app = app;

#ifdef HAVE_AYATANA_APPINDICATOR
    tray->indicator = app_indicator_new("fsearch",
                                         "io.github.cboxdoerfer.FSearch",
                                         APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(tray->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(tray->indicator, "FSearch");

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *search_item = gtk_menu_item_new_with_label(_("Search"));
    g_signal_connect(search_item, "activate", G_CALLBACK(on_tray_search_activated), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), search_item);

    GtkWidget *separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);

    GtkWidget *rescan_item = gtk_menu_item_new_with_label(_("Update Database"));
    g_signal_connect(rescan_item, "activate", G_CALLBACK(on_tray_rescan_activated), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rescan_item);

    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator2);

    GtkWidget *quit_item = gtk_menu_item_new_with_label(_("Quit"));
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_tray_quit_activated), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    app_indicator_set_menu(tray->indicator, GTK_MENU(menu));

    g_debug("[tray] appindicator tray icon created");
#else
    g_debug("[tray] built without appindicator support, no tray icon");
#endif

    return tray;
}

void
fsearch_tray_free(FsearchTray *tray) {
    if (!tray) {
        return;
    }
#ifdef HAVE_AYATANA_APPINDICATOR
    g_clear_object(&tray->indicator);
#endif
    g_free(tray);
}
