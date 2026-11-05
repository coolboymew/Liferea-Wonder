/*
 * @file install_import.h  taking over feeds and database of another Liferea
 *
 * Copyright (C) 2026 Liferea contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _INSTALL_IMPORT_H
#define _INSTALL_IMPORT_H

#include <glib.h>

/* This fork keeps its own folders, settings and database (see
   USER_DIR_NAME and SCHEMA_ID), so it can run next to upstream Liferea.
   To continue where the other installation left off, its feed list,
   database, cached icons, user stylesheet and settings can be imported.
   The database is replaced while it is closed: the import is scheduled,
   the application restarts and the files are copied on startup. The
   fork's own files are moved to a backup folder first. */

/**
 * install_import_describe_source: (skip)
 *
 * Returns: (transfer full) (nullable): a description of the other
 * installation found in the default Liferea folders, or NULL if none
 */
gchar * install_import_describe_source (void);

/**
 * install_import_schedule: (skip)
 *
 * Marks the import to be done on the next start.
 *
 * Returns: FALSE if the mark could not be written
 */
gboolean install_import_schedule (void);

/**
 * install_import_run_files: (skip)
 *
 * To be called on startup before the configuration and the database
 * are opened: does a scheduled import of the files.
 *
 * Returns: TRUE if an import was done (settings follow with
 * install_import_run_settings() once the configuration is open)
 */
gboolean install_import_run_files (void);

/**
 * install_import_run_settings: (skip)
 *
 * Copies the other installation's settings whose names and types match.
 */
void install_import_run_settings (void);

/**
 * install_import_restart: (skip)
 *
 * Quits and starts the application again once this instance is gone.
 */
void install_import_restart (void);

#endif
