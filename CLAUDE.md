# fsearch (personal fork)

Personal fork (`origin` = `skjerns/fsearch`) of upstream FSearch (`upstream` = `cboxdoerfer/fsearch`),
a fast GTK3 file search app for Linux written in C. The fork carries small UX/behaviour changes the
owner likes on top of an upstream release; it is not meant to be upstreamed as-is.

- Current base: upstream **FSearch 0.3** (`506e1ede`). Fork commits are everything in `git log 506e1ede..HEAD`.
- Old fork on top of 0.2 lives on branches `fork-0.2` / `inode_realtime` (it had its own inotify monitor
  `fsearch_monitor.c`; 0.3 ships upstream folder monitoring, so that module was dropped in the port).
- `FORK_CHANGES.md` is the porting guide written for moving the 0.2 fork onto 0.3. Some file/line
  references there describe the 0.2 fork, not the current tree. Update it when adding fork features.

## Workflow

- Commit directly to `master`, no feature branches. Push to `origin` only when asked.
- Commit identity: `Simon Kern <14980558+skjerns@users.noreply.github.com>` (already the repo's git config).
- Commit messages: imperative subject, body explaining the *why* (see recent history for style).

## Build / run / test

```sh
ninja -C builddir                 # main build dir (debug, prefix /usr/local, tray support)
./builddir/src/fsearch            # run the freshly built binary
sudo ninja -C builddir install    # installs to /usr/local/bin/fsearch (user runs this, needs sudo)
meson test -C builddir            # unit tests in src/tests/ (array, query, database, utils, ...)
```

Other dirs (`build`, `builddir_tray`, `builddir_test`) are older/stale meson dirs; prefer `builddir`.
The About dialog shows the build timestamp of `fsearch.c`, handy to check which binary is running.
Optional dep: `ayatana-appindicator3-0.1` (tray); without it tray mode is a no-op.

Desktop: KDE Plasma on Wayland. Focus/selection behaviour must work under Wayland, where keyboard focus
arrives asynchronously and KDE disables `gtk-entry-select-on-focus`.

## Code layout (src/)

- `fsearch.c` — `FsearchApplication`: startup/shutdown, CLI options, actions, tray integration.
- `fsearch_window.c` / `.ui` — main window, search entry handlers, wiring of the results list.
- `fsearch_list_view.c` — custom results widget (drawing, selection, cursor, keyboard nav, DnD, column resize).
- `fsearch_result_view.c`, `fsearch_listview_popup.c`, `menus.ui` — row rendering, context menu.
- `fsearch_window_actions.c` — window GActions (open, open folder, copy, delete, ...).
- `fsearch_query_{lexer,parser,node,matchers}.c` — query language.
- `fsearch_database_*.c`, `fsearch_folder_monitor_*.c` — index, scanning, inotify/fanotify monitoring.
- `fsearch_tray.c` — fork-only AppIndicator tray icon.

Style: 4-space indent, 120 columns (`.editorconfig`), GLib/GTK conventions, `GDK_EVENT_STOP/PROPAGATE`.

## Fork features (on top of 0.3)

- Background tray mode: `--hidden` (autostart), `--toggle` (show/hide the running instance, also hides when
  the window is in the background). Closing the window and "close after open" hide instead of quitting,
  so the index stays in memory.
- Search entry: whole query selected on focus/toggle (focus-in after-handler + `select_search_entry_on_focus`).
- Down in the search entry focuses the results and moves straight to the second row (the first row is the
  implicit current one, which Enter opens); the key event is forwarded to the list view.
- `/term` or `\term` searches `term` as a substring of the path (lexer maps `\` to `/`).
- Drag & drop of (multi-)selected results out as `text/uri-list`, started manually from the list view's
  drag gesture after the drag threshold (not `gtk_drag_source_set`, which hijacked column resizing).
- Context menu: "Open with <default text editor>" and "Copy Filepath".
- Folder monitoring defaults to ON for indexed folders; build timestamp in About.
