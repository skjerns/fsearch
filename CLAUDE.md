## Project Context: fsearch

**Note to future AI instances:** Please add any new, relevant information about the project, its structure, or user preferences to this file to maintain a persistent context.

## Project Overview

- **Project Name:** fsearch
- **Language:** C
- **Build System:** Meson
- **UI Framework:** GTK
- **Description:** Based on the file structure and content, this is a desktop application for Linux that provides fast file searching capabilities.

## Project Structure Outline

- **/src/**: Contains the core application source code, written in C (.c and .h files). This includes UI logic, database management, file utilities, and the main application entry point (`main.c`).
- **/data/**: Holds application resources such as the desktop entry (`.desktop.in.in`), icons (`.svg`), AppStream metadata (`.metainfo.xml.in`), and man pages.
- **/po/**: Manages internationalization and localization through translation files (`.po`).
- **/help/**: Contains user documentation in Mallard format (`.page` files).
- **meson.build**: The root configuration file for the Meson build system, which defines how the project is built.
- **.github/workflows/**: Contains CI/CD pipeline configurations for GitHub Actions, such as `build_test.yml`.
- **/tests/**: Contains unit tests for different modules of the application. 



# FSearch Real-Time Inotify Integration

## Goal

Extend FSearch into a tray-resident application (like Everything on Windows) that runs persistently, maintains a live in-memory file index via inotify, and shows/hides a search window on demand via a hotkey or tray icon click.

## Why a single process

Everything on Windows is a single process: it owns the MFT reader, the USN journal watcher, the tray icon, and the search window. This is the right model. Reasons:

- The database lives in-process memory. No serialization or IPC overhead for queries.
- inotify watches are tied to a file descriptor in the process. Keeping them in the same process that owns the database avoids any synchronization across process boundaries.
- GTK already supports `GtkStatusIcon` (GTK3) or `libayatana-appindicator` (GTK3/4, works on GNOME/KDE/XFCE). The tray icon is just another widget in the same main loop.
- A client/server split only pays off if multiple frontends need concurrent access (e.g. a CLI + GUI). Not needed here. If desired later, a Unix domain socket query interface can be added without restructuring.

## Architecture

```
Single process (fsearch)
├── GLib main loop
│   ├── GTK tray icon (AppIndicator / StatusNotifier)
│   ├── Search window (show/hide, not destroy/recreate)
│   └── Idle callbacks for UI refresh
├── Monitor thread
│   ├── inotify fd
│   ├── poll() loop with 200ms timeout
│   └── Event batching + coalescing
├── FsearchDatabase (in-process, shared)
│   └── Protected by GRWLock
└── Initial scanner (runs once at startup, then monitor takes over)
```

Lifecycle:
1. Process starts → tray icon appears, initial scan begins in background thread.
2. Scan completes → inotify watches registered, monitor thread starts.
3. User clicks tray icon or presses hotkey → search window toggles visibility.
4. Search window queries the in-memory database directly (read lock). No IPC.
5. Monitor thread receives inotify events → batches → updates database (write lock) → posts idle callback to refresh UI if the search window is visible.
6. User closes search window → window hides (not destroyed), process stays resident.
7. Quit from tray menu → monitor stops, watches removed, process exits.

## Implementation Plan

### 1. Tray icon integration

New files: `src/fsearch_tray.c` / `.h`

Use `libayatana-appindicator3` — it's the de facto standard for tray icons on modern Linux desktops (GNOME, KDE, XFCE, Cinnamon all support StatusNotifier/AppIndicator).

```c
#include <libayatana-appindicator/app-indicator.h>

AppIndicator *fsearch_tray_new(FsearchApplication *app) {
    AppIndicator *indicator = app_indicator_new(
        "fsearch", "fsearch", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);

    GtkWidget *menu = gtk_menu_new();
    // "Search" item — toggles window
    GtkWidget *search_item = gtk_menu_item_new_with_label("Search");
    g_signal_connect(search_item, "activate", G_CALLBACK(on_toggle_window), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), search_item);
    // "Rescan" item
    GtkWidget *rescan_item = gtk_menu_item_new_with_label("Rescan");
    g_signal_connect(rescan_item, "activate", G_CALLBACK(on_rescan), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rescan_item);
    // "Quit" item
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    return indicator;
}
```

For global hotkey (e.g. Ctrl+Space to toggle search window), use `libkeybinder-3.0` or grab keys via X11/Wayland protocol directly.

### 2. Window show/hide instead of create/destroy

Modify `FsearchWindow` lifecycle:

```c
// on toggle (tray click or hotkey)
void on_toggle_window(FsearchApplication *app) {
    GtkWidget *win = app->window;
    if (gtk_widget_get_visible(win)) {
        gtk_widget_hide(win);
    } else {
        gtk_widget_show(win);
        gtk_window_present(GTK_WINDOW(win));
        // focus the search entry
        gtk_widget_grab_focus(app->search_entry);
    }
}
```

On `delete-event` (window close button), hide instead of quitting:

```c
g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
```

### 3. Startup behavior

Modify `FsearchApplication` activation:

- First activation: create window (hidden), create tray icon, start initial scan.
- Window starts hidden. Show it only if the user launched with `--show` flag or if it's the first ever run (no database yet — show a "scanning..." window).
- Subsequent `g_application_activate` calls (user runs `fsearch` again while it's already running): just toggle the window in the existing instance. `GApplication` handles single-instance automatically.

```c
static void fsearch_application_activate(GApplication *gapp) {
    FsearchApplication *app = FSEARCH_APPLICATION(gapp);
    if (!app->window) {
        // first activation — full init
        app->window = fsearch_window_new(app);
        app->tray = fsearch_tray_new(app);
        fsearch_database_scan_async(app->db, on_scan_complete, app);
    }
    // always toggle window on activate
    on_toggle_window(app);
}
```

### 4. Monitor module

New files: `src/fsearch_monitor.c` / `.h`

```c
typedef struct {
    int inotify_fd;
    GHashTable *wd_to_path;   // watch descriptor -> directory path
    GHashTable *path_to_wd;   // directory path -> watch descriptor
    GThread *thread;
    gboolean running;         // atomic, signals shutdown
    FsearchDatabase *db;
    GRWLock *db_lock;
} FsearchMonitor;
```

Public API:

```c
FsearchMonitor *fsearch_monitor_new(FsearchDatabase *db, GRWLock *db_lock);
void fsearch_monitor_add_recursive(FsearchMonitor *mon, const char *root);
void fsearch_monitor_start(FsearchMonitor *mon);
void fsearch_monitor_stop(FsearchMonitor *mon);
void fsearch_monitor_free(FsearchMonitor *mon);
```

### 5. Watch setup

After initial scan completes, iterate all directories in the database:

```c
int wd = inotify_add_watch(mon->inotify_fd, dir_path,
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB);
```

Store bidirectional `wd <-> path` mappings. Do this in a background thread — the database is already queryable while watches are being registered.

**Race condition**: When `IN_CREATE` fires for a new directory, add a watch for it, then scan its contents. Files created between `mkdir` and the watch being established are caught by the post-watch scan.

### 6. Watcher thread loop

```
while (mon->running):
    poll(inotify_fd, timeout=200ms)
    if events available:
        read all pending events into buffer
        coalesce events (see batching)
        take db write lock
        apply updates to FsearchDatabase
        release db write lock
        g_idle_add(refresh_callback, NULL)
```

### 7. Event batching

Buffer events for ~200ms. Coalescing rules:

- CREATE then DELETE of same path → discard both (transient file)
- DELETE then CREATE of same path → treat as MODIFY
- MOVED_FROM + MOVED_TO with same cookie → single rename
- Multiple MODIFY on same path → collapse to one

### 8. Database updates

For each coalesced event:

- **CREATE (file)**: Insert `FsearchDatabaseEntry`, populate via `stat()`.
- **CREATE (directory)**: Insert entry, add inotify watch, scan contents recursively.
- **DELETE**: Remove entry. If directory, remove watch + all children.
- **RENAME**: Update path in-place. Cross-directory moves → delete + create.
- **MODIFY**: Update size/mtime.

All mutations under write lock. Prepare entries outside the lock, insert/remove under it.

### 9. Locking

`GRWLock` shared between monitor thread and UI/query thread:

- Monitor thread: `g_rw_lock_writer_lock` when mutating.
- Query/UI: `g_rw_lock_reader_lock` when searching/rendering.

With batching, write locks fire at most ~5 times/second during normal use.

### 10. UI refresh

After applying a batch, post to GTK main loop:

```c
g_idle_add((GSourceFunc)fsearch_ui_invalidate_results, app);
```

The callback checks if the search window is visible and if the current query matches changed paths. No-op if window is hidden — the database is still current, and results will be correct when the window is shown again.

### 11. System configuration

```bash
# Check current inotify watch limit
cat /proc/sys/fs/inotify/max_user_watches

# Raise temporarily
sudo sysctl fs.inotify.max_user_watches=1048576

# Raise permanently
echo 'fs.inotify.max_user_watches=1048576' | sudo tee -a /etc/sysctl.d/99-fsearch.conf
sudo sysctl --system
```

If `inotify_add_watch` returns `ENOSPC`, degrade gracefully — database works, just no real-time updates for unwatched dirs. Show a non-blocking notification via the tray icon.

## Performance Budget

| Metric | Expected |
|---|---|
| Kernel memory per watch | ~1 KB |
| 500k directories watched | ~500 MB kernel memory |
| Idle CPU (no FS changes) | 0% |
| Resident memory (database, 1M files) | ~200-400 MB |
| Burst I/O (git clone, package install) | <5% CPU spike with batching |
| Watch setup time (500k dirs) | 3-8 seconds in background thread |
| Write lock hold time per batch | <1 ms |

## Files to Create / Modify

New:
- `src/fsearch_monitor.c` / `.h` — inotify watcher thread, event batching, database updates
- `src/fsearch_tray.c` / `.h` — AppIndicator tray icon, context menu

Modify:
- `src/fsearch_database.c` — expose insert/remove/update entry functions, add `GRWLock`
- `src/fsearch_application.c` — single-instance lifecycle, tray init, monitor start/stop, window show/hide logic
- `src/fsearch_window.c` — hide-on-close instead of quit, idle refresh callback
- `meson.build` — add new source files, add dependencies (`ayatana-appindicator3-0.1`, optionally `keybinder-3.0`)

## Dependencies

```bash
# Debian/Ubuntu
sudo apt install libayatana-appindicator3-dev libkeybinder-3.0-dev

# Fedora
sudo dnf install libayatana-appindicator-gtk3-devel keybinder3-devel

# Arch
sudo pacman -S libayatana-appindicator keybinder3
```

## Build and Test

```bash
git clone https://github.com/cboxdoerfer/fsearch.git
cd fsearch
# apply changes
meson setup build
cd build
meson compile
./src/fsearch  # starts in tray, scan begins
# test: click tray icon to open search, create/delete/rename files, observe live updates
# test: close window (should hide, not quit), re-open via tray
# test: run `fsearch` again — should raise existing window (single instance)
```

## Edge Cases

- **Symlinks**: Don't follow for watch setup (avoids cycles). Index the symlink entry itself.
- **Mount points**: inotify doesn't cross filesystem boundaries. Document this.
- **Permissions**: Skip unreadable directories silently, log at debug level.
- **IN_Q_OVERFLOW**: Kernel inotify queue overflowed. Trigger full re-scan.
- **IN_UNMOUNT**: Filesystem unmounted. Remove corresponding entries from database.
- **Desktop environment differences**: AppIndicator support varies. Fall back to `GtkStatusIcon` (deprecated but functional) if AppIndicator is unavailable. On Wayland-only compositors without tray support, the application may need to stay as a normal window — detect this at runtime.
- **Autostart**: Provide a `.desktop` file for `~/.config/autostart/` so the process starts at login.

## Autostart Desktop Entry

```ini
[Desktop Entry]
Type=Application
Name=FSearch
Comment=Real-time file search
Exec=fsearch --hidden
Icon=fsearch
Terminal=false
X-GNOME-Autostart-enabled=true
```

Place in `~/.config/autostart/fsearch.desktop`. The `--hidden` flag starts the process without showing the search window.
