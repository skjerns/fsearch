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

#define G_LOG_DOMAIN "fsearch-monitor"

#include "fsearch_monitor.h"
#include "fsearch_database.h"
#include "fsearch_database_entry.h"

#include <errno.h>
#include <glib.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define INOTIFY_WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF)
#define BATCH_TIMEOUT_MS 200
#define INOTIFY_BUF_SIZE (4096 * (sizeof(struct inotify_event) + NAME_MAX + 1))

typedef enum {
    MONITOR_EVENT_CREATE,
    MONITOR_EVENT_DELETE,
    MONITOR_EVENT_MODIFY,
    MONITOR_EVENT_RENAME,
} MonitorEventType;

typedef struct {
    MonitorEventType type;
    char *path;           // full path of the affected file/dir
    char *new_path;       // for renames: the new path
    gboolean is_dir;
} MonitorEvent;

struct FsearchMonitor {
    int inotify_fd;
    GHashTable *wd_to_path;       // int watch descriptor -> char* dir path (owned)
    GHashTable *path_to_wd;       // char* dir path -> GINT_TO_POINTER(wd)
    GHashTable *path_to_entry;    // char* full path -> FsearchDatabaseEntry*
    GThread *thread;
    volatile gboolean running;
    FsearchDatabase *db;
    GQueue *pending_events;
    GMutex batch_mutex;
    guint batch_timeout_id;

    // For move coalescing
    GHashTable *pending_moves;    // uint32_t cookie -> MonitorEvent* (MOVED_FROM waiting for MOVED_TO)
};

static void
monitor_event_free(MonitorEvent *event) {
    if (!event) return;
    g_free(event->path);
    g_free(event->new_path);
    g_free(event);
}

static MonitorEvent *
monitor_event_new(MonitorEventType type, const char *path, gboolean is_dir) {
    MonitorEvent *event = g_new0(MonitorEvent, 1);
    event->type = type;
    event->path = g_strdup(path);
    event->is_dir = is_dir;
    return event;
}

static void
build_path_to_entry_map(FsearchMonitor *mon) {
    g_hash_table_remove_all(mon->path_to_entry);

    // Map all folders
    DynamicArray *folders = db_get_folders(mon->db);
    if (folders) {
        uint32_t num = darray_get_num_items(folders);
        for (uint32_t i = 0; i < num; i++) {
            FsearchDatabaseEntry *entry = darray_get_item(folders, i);
            if (!entry) continue;
            GString *path = db_entry_get_path_full(entry);
            if (path) {
                // Remove trailing slash if present
                if (path->len > 1 && path->str[path->len - 1] == G_DIR_SEPARATOR) {
                    g_string_truncate(path, path->len - 1);
                }
                g_hash_table_insert(mon->path_to_entry, g_string_free(path, FALSE), entry);
            }
        }
        darray_unref(folders);
    }

    // Map all files
    DynamicArray *files = db_get_files(mon->db);
    if (files) {
        uint32_t num = darray_get_num_items(files);
        for (uint32_t i = 0; i < num; i++) {
            FsearchDatabaseEntry *entry = darray_get_item(files, i);
            if (!entry) continue;
            GString *path = db_entry_get_path_full(entry);
            if (path) {
                g_hash_table_insert(mon->path_to_entry, g_string_free(path, FALSE), entry);
            }
        }
        darray_unref(files);
    }
}

static void
add_watch_for_dir(FsearchMonitor *mon, const char *dir_path) {
    if (g_hash_table_contains(mon->path_to_wd, dir_path)) {
        return;
    }

    int wd = inotify_add_watch(mon->inotify_fd, dir_path, INOTIFY_WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) {
            g_warning("[monitor] inotify watch limit reached, cannot watch: %s", dir_path);
        } else {
            g_debug("[monitor] failed to add watch for %s: %s", dir_path, g_strerror(errno));
        }
        return;
    }

    char *path_copy = g_strdup(dir_path);
    char *path_copy2 = g_strdup(dir_path);
    g_hash_table_insert(mon->wd_to_path, GINT_TO_POINTER(wd), path_copy);
    g_hash_table_insert(mon->path_to_wd, path_copy2, GINT_TO_POINTER(wd));
}

static void
remove_watch_for_dir(FsearchMonitor *mon, const char *dir_path) {
    gpointer wd_ptr = NULL;
    if (!g_hash_table_lookup_extended(mon->path_to_wd, dir_path, NULL, &wd_ptr)) {
        return;
    }
    int wd = GPOINTER_TO_INT(wd_ptr);
    inotify_rm_watch(mon->inotify_fd, wd);
    g_hash_table_remove(mon->wd_to_path, GINT_TO_POINTER(wd));
    g_hash_table_remove(mon->path_to_wd, dir_path);
}

static gboolean
apply_batch_idle(gpointer user_data) {
    FsearchMonitor *mon = user_data;

    g_mutex_lock(&mon->batch_mutex);
    GQueue *events = mon->pending_events;
    mon->pending_events = g_queue_new();
    mon->batch_timeout_id = 0;

    // Also collect any pending moves that never got a MOVED_TO
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mon->pending_moves);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MonitorEvent *orphan = value;
        orphan->type = MONITOR_EVENT_DELETE;
        g_queue_push_tail(events, orphan);
        g_hash_table_iter_steal(&iter);
    }
    g_mutex_unlock(&mon->batch_mutex);

    if (g_queue_is_empty(events)) {
        g_queue_free(events);
        return G_SOURCE_REMOVE;
    }

    // Coalesce events
    GHashTable *event_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    GQueue *coalesced = g_queue_new();

    MonitorEvent *event;
    while ((event = g_queue_pop_head(events)) != NULL) {
        MonitorEvent *existing = g_hash_table_lookup(event_map, event->path);
        if (existing) {
            // CREATE then DELETE = discard both
            if (existing->type == MONITOR_EVENT_CREATE && event->type == MONITOR_EVENT_DELETE) {
                g_hash_table_remove(event_map, existing->path);
                g_queue_remove(coalesced, existing);
                monitor_event_free(existing);
                monitor_event_free(event);
                continue;
            }
            // DELETE then CREATE = MODIFY
            if (existing->type == MONITOR_EVENT_DELETE && event->type == MONITOR_EVENT_CREATE) {
                existing->type = MONITOR_EVENT_MODIFY;
                monitor_event_free(event);
                continue;
            }
            // Multiple MODIFY = keep one
            if (existing->type == MONITOR_EVENT_MODIFY && event->type == MONITOR_EVENT_MODIFY) {
                monitor_event_free(event);
                continue;
            }
            // Replace existing with new event
            g_queue_remove(coalesced, existing);
            monitor_event_free(existing);
        }
        g_hash_table_insert(event_map, event->path, event);
        g_queue_push_tail(coalesced, event);
    }
    g_queue_free(events);
    g_hash_table_destroy(event_map);

    // Apply coalesced events to database
    db_lock(mon->db);

    while ((event = g_queue_pop_head(coalesced)) != NULL) {
        switch (event->type) {
        case MONITOR_EVENT_CREATE: {
            struct stat st;
            if (stat(event->path, &st) != 0) {
                g_debug("[monitor] stat failed for %s: %s", event->path, g_strerror(errno));
                monitor_event_free(event);
                continue;
            }

            char *name = g_path_get_basename(event->path);
            char *dir = g_path_get_dirname(event->path);

            // Find parent folder in our map
            FsearchDatabaseEntry *parent_entry = g_hash_table_lookup(mon->path_to_entry, dir);

            if (S_ISDIR(st.st_mode)) {
                FsearchDatabaseEntry *entry = db_alloc_folder_entry(mon->db);
                db_entry_set_name(entry, name);
                db_entry_set_mtime(entry, st.st_mtime);
                if (parent_entry && db_entry_is_folder(parent_entry)) {
                    db_entry_set_parent(entry, (FsearchDatabaseEntryFolder *)parent_entry);
                }
                db_insert_folder_entry(mon->db, entry);
                g_hash_table_insert(mon->path_to_entry, g_strdup(event->path), entry);

                // Add inotify watch for new directory
                add_watch_for_dir(mon, event->path);

                g_debug("[monitor] created folder: %s", event->path);
            } else {
                FsearchDatabaseEntry *entry = db_alloc_file_entry(mon->db);
                db_entry_set_name(entry, name);
                db_entry_set_size(entry, st.st_size);
                db_entry_set_mtime(entry, st.st_mtime);
                if (parent_entry && db_entry_is_folder(parent_entry)) {
                    db_entry_set_parent(entry, (FsearchDatabaseEntryFolder *)parent_entry);
                }
                db_insert_file_entry(mon->db, entry);
                g_hash_table_insert(mon->path_to_entry, g_strdup(event->path), entry);

                g_debug("[monitor] created file: %s", event->path);
            }

            g_free(name);
            g_free(dir);
            break;
        }

        case MONITOR_EVENT_DELETE: {
            FsearchDatabaseEntry *entry = g_hash_table_lookup(mon->path_to_entry, event->path);
            if (entry) {
                if (db_entry_is_folder(entry)) {
                    db_remove_folder_entry(mon->db, entry);
                    remove_watch_for_dir(mon, event->path);
                } else {
                    db_remove_file_entry(mon->db, entry);
                }
                g_hash_table_remove(mon->path_to_entry, event->path);
                g_debug("[monitor] deleted: %s", event->path);
            }
            break;
        }

        case MONITOR_EVENT_MODIFY: {
            FsearchDatabaseEntry *entry = g_hash_table_lookup(mon->path_to_entry, event->path);
            if (entry) {
                struct stat st;
                if (stat(event->path, &st) == 0) {
                    // Remove from sorted arrays, update, re-insert
                    if (db_entry_is_file(entry)) {
                        db_remove_file_entry(mon->db, entry);
                        db_entry_set_size(entry, st.st_size);
                        db_entry_set_mtime(entry, st.st_mtime);
                        db_insert_file_entry(mon->db, entry);
                    } else {
                        db_remove_folder_entry(mon->db, entry);
                        db_entry_set_mtime(entry, st.st_mtime);
                        db_insert_folder_entry(mon->db, entry);
                    }
                    g_debug("[monitor] modified: %s", event->path);
                }
            }
            break;
        }

        case MONITOR_EVENT_RENAME: {
            FsearchDatabaseEntry *entry = g_hash_table_lookup(mon->path_to_entry, event->path);
            if (entry) {
                char *new_name = g_path_get_basename(event->new_path);
                char *new_dir = g_path_get_dirname(event->new_path);

                if (db_entry_is_file(entry)) {
                    db_remove_file_entry(mon->db, entry);
                    db_entry_set_name(entry, new_name);
                    // Check if directory changed
                    FsearchDatabaseEntry *new_parent = g_hash_table_lookup(mon->path_to_entry, new_dir);
                    if (new_parent && db_entry_is_folder(new_parent)) {
                        db_entry_set_parent(entry, (FsearchDatabaseEntryFolder *)new_parent);
                    }
                    db_insert_file_entry(mon->db, entry);
                } else {
                    db_remove_folder_entry(mon->db, entry);
                    db_entry_set_name(entry, new_name);
                    FsearchDatabaseEntry *new_parent = g_hash_table_lookup(mon->path_to_entry, new_dir);
                    if (new_parent && db_entry_is_folder(new_parent)) {
                        db_entry_set_parent(entry, (FsearchDatabaseEntryFolder *)new_parent);
                    }
                    db_insert_folder_entry(mon->db, entry);

                    // Update watch
                    remove_watch_for_dir(mon, event->path);
                    add_watch_for_dir(mon, event->new_path);
                }

                g_hash_table_remove(mon->path_to_entry, event->path);
                g_hash_table_insert(mon->path_to_entry, g_strdup(event->new_path), entry);

                g_debug("[monitor] renamed: %s -> %s", event->path, event->new_path);

                g_free(new_name);
                g_free(new_dir);
            }
            break;
        }
        }

        monitor_event_free(event);
    }

    db_unlock(mon->db);
    g_queue_free(coalesced);

    db_notify_views_content_changed(mon->db);

    return G_SOURCE_REMOVE;
}

static void
schedule_batch_apply(FsearchMonitor *mon) {
    if (mon->batch_timeout_id == 0) {
        mon->batch_timeout_id = g_timeout_add(BATCH_TIMEOUT_MS, apply_batch_idle, mon);
    }
}

static void
process_inotify_event(FsearchMonitor *mon, const struct inotify_event *event) {
    if (!event->len) {
        // Events without a name (e.g. IN_DELETE_SELF on the watched dir itself)
        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            char *dir_path = g_hash_table_lookup(mon->wd_to_path, GINT_TO_POINTER(event->wd));
            if (dir_path) {
                g_mutex_lock(&mon->batch_mutex);
                MonitorEvent *me = monitor_event_new(MONITOR_EVENT_DELETE, dir_path, TRUE);
                g_queue_push_tail(mon->pending_events, me);
                schedule_batch_apply(mon);
                g_mutex_unlock(&mon->batch_mutex);
            }
        }
        return;
    }

    char *dir_path = g_hash_table_lookup(mon->wd_to_path, GINT_TO_POINTER(event->wd));
    if (!dir_path) {
        return;
    }

    char *full_path = g_build_filename(dir_path, event->name, NULL);
    gboolean is_dir = (event->mask & IN_ISDIR) != 0;

    g_mutex_lock(&mon->batch_mutex);

    if (event->mask & IN_Q_OVERFLOW) {
        g_warning("[monitor] inotify queue overflow, full rescan needed");
        // TODO: trigger full rescan
        g_mutex_unlock(&mon->batch_mutex);
        g_free(full_path);
        return;
    }

    if (event->mask & IN_CREATE) {
        MonitorEvent *me = monitor_event_new(MONITOR_EVENT_CREATE, full_path, is_dir);
        g_queue_push_tail(mon->pending_events, me);
    }

    if (event->mask & IN_DELETE) {
        MonitorEvent *me = monitor_event_new(MONITOR_EVENT_DELETE, full_path, is_dir);
        g_queue_push_tail(mon->pending_events, me);
    }

    if (event->mask & IN_MODIFY) {
        MonitorEvent *me = monitor_event_new(MONITOR_EVENT_MODIFY, full_path, is_dir);
        g_queue_push_tail(mon->pending_events, me);
    }

    if (event->mask & IN_ATTRIB) {
        MonitorEvent *me = monitor_event_new(MONITOR_EVENT_MODIFY, full_path, is_dir);
        g_queue_push_tail(mon->pending_events, me);
    }

    if (event->mask & IN_MOVED_FROM) {
        MonitorEvent *me = monitor_event_new(MONITOR_EVENT_DELETE, full_path, is_dir);
        g_hash_table_insert(mon->pending_moves, GUINT_TO_POINTER(event->cookie), me);
    }

    if (event->mask & IN_MOVED_TO) {
        MonitorEvent *from_event = g_hash_table_lookup(mon->pending_moves, GUINT_TO_POINTER(event->cookie));
        if (from_event) {
            // Matched MOVED_FROM + MOVED_TO = rename
            from_event->type = MONITOR_EVENT_RENAME;
            from_event->new_path = g_strdup(full_path);
            g_queue_push_tail(mon->pending_events, from_event);
            g_hash_table_steal(mon->pending_moves, GUINT_TO_POINTER(event->cookie));
        } else {
            // MOVED_TO without MOVED_FROM = treat as create (moved from outside)
            MonitorEvent *me = monitor_event_new(MONITOR_EVENT_CREATE, full_path, is_dir);
            g_queue_push_tail(mon->pending_events, me);
        }
    }

    schedule_batch_apply(mon);
    g_mutex_unlock(&mon->batch_mutex);

    g_free(full_path);
}

static gpointer
monitor_thread_func(gpointer data) {
    FsearchMonitor *mon = data;

    g_debug("[monitor] watcher thread started");

    char buf[INOTIFY_BUF_SIZE] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (g_atomic_int_get(&mon->running)) {
        struct pollfd pfd = {
            .fd = mon->inotify_fd,
            .events = POLLIN,
        };

        int ret = poll(&pfd, 1, BATCH_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;
            g_warning("[monitor] poll error: %s", g_strerror(errno));
            break;
        }

        if (ret == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        ssize_t len = read(mon->inotify_fd, buf, sizeof(buf));
        if (len <= 0) {
            if (errno == EINTR) continue;
            if (len == 0) break;
            g_warning("[monitor] read error: %s", g_strerror(errno));
            break;
        }

        const char *ptr = buf;
        while (ptr < buf + len) {
            const struct inotify_event *event = (const struct inotify_event *)ptr;
            process_inotify_event(mon, event);
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    g_debug("[monitor] watcher thread stopped");
    return NULL;
}

FsearchMonitor *
fsearch_monitor_new(FsearchDatabase *db) {
    FsearchMonitor *mon = g_new0(FsearchMonitor, 1);

    mon->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (mon->inotify_fd < 0) {
        g_warning("[monitor] failed to initialize inotify: %s", g_strerror(errno));
        g_free(mon);
        return NULL;
    }

    mon->db = db_ref(db);
    mon->wd_to_path = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    mon->path_to_wd = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    mon->path_to_entry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    mon->pending_events = g_queue_new();
    mon->pending_moves = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    g_mutex_init(&mon->batch_mutex);
    mon->running = FALSE;

    g_debug("[monitor] created");
    return mon;
}

void
fsearch_monitor_watch_database_folders(FsearchMonitor *mon) {
    g_return_if_fail(mon);

    g_autoptr(GTimer) timer = g_timer_new();

    // Build path-to-entry lookup map
    build_path_to_entry_map(mon);

    // Add watches for all directories in the database
    DynamicArray *folders = db_get_folders(mon->db);
    if (!folders) {
        g_debug("[monitor] no folders in database");
        return;
    }

    uint32_t num_folders = darray_get_num_items(folders);
    uint32_t num_watches = 0;

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntry *entry = darray_get_item(folders, i);
        if (!entry) continue;

        GString *path = db_entry_get_path_full(entry);
        if (!path) continue;

        // Remove trailing slash for inotify
        if (path->len > 1 && path->str[path->len - 1] == G_DIR_SEPARATOR) {
            g_string_truncate(path, path->len - 1);
        }

        add_watch_for_dir(mon, path->str);
        num_watches++;

        g_string_free(path, TRUE);
    }

    darray_unref(folders);

    const double seconds = g_timer_elapsed(timer, NULL);
    g_debug("[monitor] added %u watches for %u folders in %.2f ms",
            g_hash_table_size(mon->wd_to_path), num_folders, seconds * 1000);
}

void
fsearch_monitor_start(FsearchMonitor *mon) {
    g_return_if_fail(mon);

    if (g_atomic_int_get(&mon->running)) {
        g_debug("[monitor] already running");
        return;
    }

    g_atomic_int_set(&mon->running, TRUE);
    mon->thread = g_thread_new("fsearch-monitor", monitor_thread_func, mon);

    g_debug("[monitor] started");
}

void
fsearch_monitor_stop(FsearchMonitor *mon) {
    g_return_if_fail(mon);

    if (!g_atomic_int_get(&mon->running)) {
        return;
    }

    g_atomic_int_set(&mon->running, FALSE);

    if (mon->thread) {
        g_thread_join(mon->thread);
        mon->thread = NULL;
    }

    // Cancel pending batch
    if (mon->batch_timeout_id) {
        g_source_remove(mon->batch_timeout_id);
        mon->batch_timeout_id = 0;
    }

    g_debug("[monitor] stopped");
}

void
fsearch_monitor_free(FsearchMonitor *mon) {
    if (!mon) return;

    fsearch_monitor_stop(mon);

    if (mon->inotify_fd >= 0) {
        close(mon->inotify_fd);
    }

    g_clear_pointer(&mon->wd_to_path, g_hash_table_destroy);
    g_clear_pointer(&mon->path_to_wd, g_hash_table_destroy);
    g_clear_pointer(&mon->path_to_entry, g_hash_table_destroy);

    // Free pending events
    if (mon->pending_events) {
        MonitorEvent *event;
        while ((event = g_queue_pop_head(mon->pending_events)) != NULL) {
            monitor_event_free(event);
        }
        g_queue_free(mon->pending_events);
    }

    // Free pending moves
    if (mon->pending_moves) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, mon->pending_moves);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            monitor_event_free(value);
        }
        g_hash_table_destroy(mon->pending_moves);
    }

    g_mutex_clear(&mon->batch_mutex);
    g_clear_pointer(&mon->db, db_unref);

    g_free(mon);
    g_debug("[monitor] freed");
}
