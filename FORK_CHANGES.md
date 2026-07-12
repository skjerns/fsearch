# Fork changes — porting guide

A summary of everything this fork adds on top of upstream `cboxdoerfer/fsearch`,
and briefly how each is implemented, so the changes can be re-applied selectively
onto a newer upstream release.

Fork base: diverges from upstream at commit `463d7af`. All commits listed under
each feature are on `master`.

> **Note on diffing:** the working tree/history contains a line-ending
> normalization ("whitespace stuff", commit `c27f179`) that makes a raw
> `git diff upstream/master...HEAD` extremely noisy. When porting, cherry-pick or
> re-apply per-feature using the files listed below rather than one big diff.

---

## New files (drop-in modules)

| File | Purpose |
|---|---|
| `src/fsearch_tray.c` / `.h` | AppIndicator tray icon + context menu (~113 lines) |
| `src/fsearch_monitor.c` / `.h` | inotify real-time index monitor, event batching/coalescing (~868 lines) |

Both are added to `src/meson.build` (`libfsearch_sources`).

---

## 1. Real-time inotify monitoring
Commits: `1d16f6f` (initial), `a2ea724` (dedupe/warnings), `532cfb3` (ENOSPC)

Keeps the in-memory database live as the filesystem changes — no manual rescan.

**Implementation** (`src/fsearch_monitor.c`):
- One background thread runs `monitor_thread_func` with `poll()` on an inotify fd.
- Watch mask: `IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF`.
- `fsearch_monitor_watch_database_folders()` walks the DB's indexed folders and adds a watch per directory (`add_watch_for_dir`), keeping `wd <-> path` maps.
- Events are buffered and flushed via a `batch_timeout_id` timeout; `apply_batch_idle()` coalesces (CREATE+DELETE cancel, MOVED_FROM/MOVED_TO by cookie = rename, collapse duplicate MODIFY) then applies to the DB on the GTK main loop.
- New directories: add watch, then scan contents (races handled by skipping already-known entries).
- Public API: `fsearch_monitor_new(db)`, `_watch_database_folders`, `_start`, `_stop`, `_free`.

**Integration** (`src/fsearch.c`, ~line 225): after a DB scan/load completes, the app
frees any old monitor, creates a new one, watches folders, and starts it. Stopped
first in `shutdown` (~line 630).

**ENOSPC handling** (`532cfb3`): on hitting the watch limit, sets a
`watch_limit_reached` flag to stop retrying and logs a one-time actionable message
(`sudo sysctl fs.inotify.max_user_watches=524288`).

---

## 2. Tray icon + background-resident lifecycle
Commit: `1d16f6f`

App stays running in the system tray; the search window is shown/hidden, not
created/destroyed.

**Implementation**:
- `src/fsearch_tray.c` uses `libayatana-appindicator` (`#include <libayatana-appindicator/app-indicator.h>`). `fsearch_tray_new(app)` builds an `AppIndicator` with a GTK menu (toggle window / quit etc). Optional dep — guarded by `ayatana_dep.found()` in meson.
- `src/fsearch.c`: `FsearchApplication` gains `FsearchTray *tray` and `FsearchMonitor *monitor` fields. Tray created in `startup` (~line 767), freed in `shutdown` (~line 648).
- Window close / Escape / Ctrl+Q **hide** the window instead of quitting (only tray "Quit" actually exits). See `~line 694`.

**meson**: add `ayatana_dep = dependency('ayatana-appindicator3-0.1', required: false)`
and append to `fsearch_deps` when found. (Check top of `src/meson.build` for the
`ayatana_dep` definition when porting.)

---

## 3. Command-line / single-instance controls
Commits: `1d16f6f`, `eec376f`, `449c5e7`

New `G_OPTION` entries in `src/fsearch.c` (`~line 1028`) handled in
`command_line`/`activate`:
- `--toggle` — show window if hidden, hide if visible, talking to the running instance (`~line 830`).
- `--hidden` — start with the window hidden (for autostart) (`~line 851`).
- `fsearch_application_toggle_window()` implements the show/hide + focus logic.

**Autostart** (`~line 523`): a preference writes/removes
`~/.config/autostart/fsearch.desktop` (`Exec=fsearch --hidden`).

---

## 4. Reliable search-entry selection on toggle (Wayland/KDE)
Commits: `d38eb94`, `e1d038c`, `993fc8c`

On show/restore the previous query is fully selected so typing replaces it.

**Implementation** (`src/fsearch_window.c`):
- Struct field `gboolean select_search_entry_on_focus`.
- `fsearch_application_window_focus_search_entry()` selects synchronously **and** arms the flag.
- `on_search_entry_focus_in_event` (connected with `g_signal_connect_after` on `search_entry`'s `focus-in-event`) re-applies `gtk_editable_select_region(…, 0, -1)` when the flag is set.
- Rationale: Wayland delivers keyboard focus asynchronously and KDE disables `gtk-entry-select-on-focus`, so a synchronous-only select gets clobbered when focus lands. The after-handler wins.

---

## 5. Wayland crash fix on window present
Commit: `adae8db`

**Problem**: `gdk_x11_get_server_time()` was guarded only by the compile-time
`#ifdef GDK_WINDOWING_X11`, which is defined even under Wayland (GTK built with both
backends) → SIGSEGV.

**Fix** (`src/fsearch.c`): helper `fsearch_present_window()` does a **runtime**
`GDK_IS_X11_DISPLAY(...)` check before using the X11 timestamp path, else falls back
to `gtk_window_present()`. Routed all present call sites through it.

---

## 6. `/term` and `\term` folder-search prefix syntax
Commit: `f7d46bb`

Typing `/foo` (or `\foo`) searches `foo` as a substring of the directory path.

**Implementation**:
- `src/fsearch_query_lexer.c` (`~line 134`): backslash `\` is lexed as `/`.
- `src/fsearch_query_parser.c` (`~line 478`): a field token starting with `/`
  (and not already a path search) is turned into a path substring search.

---

## 7. Exact-match quoted search
Commit: `1d16f6f`

`QUERY_FLAG_EXACT_MATCH` support in the parser (`src/fsearch_query_parser.c`,
`{"exact", QUERY_FLAG_EXACT_MATCH, ADD_FLAG}`) and query node matching.

---

## 8. Drag & drop out of results
Commits: `f07933f` / `1f7c747` / `34dd145`

Drag selected files from the results list into other apps.

**Implementation** (`src/fsearch_window.c` `init_listview`, `~line 885`):
- `gtk_drag_source_set(list_view, GDK_BUTTON1_MASK, {"text/uri-list"}, …, GDK_ACTION_COPY)`.
- `on_listview_drag_begin` (`~line 477`) and `on_listview_drag_data_get` (`~line 517`)
  supply the selected rows as a `text/uri-list`.

---

## 9. Double-click Location column opens folder
Commit: `c16cc4d`

Small change in `src/fsearch_window.c` row-activated handling: activating a row via
the Location/Path column opens the containing folder rather than the file.

---

## 10. High-CPU-after-resume fix
Commit: `468f0a5`

Fixes a CPU spin after suspend/resume; touches `fsearch.c`, `fsearch_monitor.c`
(re-arm/rescan logic after resume), `fsearch_list_view.c`, and adds a
`bool load_on_startup` config option (`src/fsearch_config.h`) surfaced in
Preferences (`fsearch_preferences.ui` / `_ui.c`).

---

## 11. First-row auto-select fix
Commit: `5d2a329`

Fixes the first result not getting selected when the list updates from inotify
events (`src/fsearch_list_view.c` / window result handling).

---

## 12. Build timestamp in About dialog
Commit: `993fc8c`

`action_about_activated` (`src/fsearch.c` `~line 474`) appends
`\nBuilt: <__DATE__> <__TIME__>` to the version string so the running binary can be
identified. (Reflects when `fsearch.c` was compiled.)

---

## Suggested porting order onto a new upstream release

1. Copy in `fsearch_tray.*` and `fsearch_monitor.*`; wire `meson.build` (sources + optional `ayatana_dep`).
2. Apply `fsearch.c` integration: struct fields, startup/shutdown tray+monitor, `--toggle`/`--hidden`, hide-on-close, autostart, `fsearch_present_window` (Wayland fix), About build-time.
3. Apply `fsearch_window.c`: drag&drop, double-click, focus/select-on-toggle handler.
4. Apply query changes: lexer backslash + parser `/term` prefix + `exact` flag.
5. Apply resume/first-row fixes and the `load_on_startup` config + Preferences UI.
6. Rebuild, verify About shows the new build time, test toggle/selection under your compositor.
