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

#pragma once

#include "fsearch_database.h"
#include <glib.h>

typedef struct FsearchMonitor FsearchMonitor;

FsearchMonitor *
fsearch_monitor_new(FsearchDatabase *db);

void
fsearch_monitor_watch_database_folders(FsearchMonitor *mon);

void
fsearch_monitor_start(FsearchMonitor *mon);

void
fsearch_monitor_stop(FsearchMonitor *mon);

void
fsearch_monitor_free(FsearchMonitor *mon);
