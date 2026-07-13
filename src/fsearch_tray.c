/*
   FSearch - A fast file search utility
   Copyright © 2026 Christian Boxdörfer

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

#include <glib/gi18n.h>
#include <gtk/gtk.h>

struct FsearchTray {
#ifdef HAVE_AYATANA_APPINDICATOR
    AppIndicator *indicator;
#endif
    FsearchApplication *app;
};

#ifdef HAVE_AYATANA_APPINDICATOR
static void
on_tray_toggle_activated(GtkMenuItem *item, gpointer user_data) {
    FsearchApplication *app = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(app), "toggle_window", NULL);
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

static void
append_menu_item(GtkMenuShell *menu, const char *label, GCallback cb, gpointer user_data) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, user_data);
    gtk_menu_shell_append(menu, item);
}
#endif

FsearchTray *
fsearch_tray_new(FsearchApplication *app) {
    FsearchTray *tray = g_new0(FsearchTray, 1);
    tray->app = app;

#ifdef HAVE_AYATANA_APPINDICATOR
    // app_indicator_new is marked deprecated by libayatana but is still the
    // supported way to create an indicator; silence the cosmetic warning.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    tray->indicator = app_indicator_new("fsearch", APP_ID, APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app_indicator_set_status(tray->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(tray->indicator, "FSearch");

    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    append_menu_item(menu, _("Show / Hide"), G_CALLBACK(on_tray_toggle_activated), app);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    append_menu_item(menu, _("Update Database"), G_CALLBACK(on_tray_rescan_activated), app);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    append_menu_item(menu, _("Quit"), G_CALLBACK(on_tray_quit_activated), app);
    gtk_widget_show_all(GTK_WIDGET(menu));
    app_indicator_set_menu(tray->indicator, GTK_MENU(menu));

    g_debug("[tray] appindicator tray icon created");
#else
    g_debug("[tray] built without appindicator support, no tray icon");
#endif

    return tray;
}

bool
fsearch_tray_is_available(FsearchTray *tray) {
#ifdef HAVE_AYATANA_APPINDICATOR
    return tray && tray->indicator != NULL;
#else
    return false;
#endif
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
