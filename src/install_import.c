/**
 * @file install_import.c  taking over feeds and database of another Liferea
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

#include "install_import.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "debug.h"

/* folder name and settings schema of upstream Liferea */
#define UPSTREAM_DIR_NAME	"liferea"
#define UPSTREAM_SCHEMA_ID	"net.sf.liferea"

#define IMPORT_MARKER		"import-pending"

static gboolean importedFiles = FALSE;

/* Inside a Flatpak the user folders are the app's own (~/.var/app/...),
   regular Liferea's are in the real ones (readable through the Flatpak's
   permissions); HOST_XDG_* name them when moved */
static const gchar *
install_import_host_dir (const gchar *base)
{
	static gchar *dirs[3] = { NULL, NULL, NULL };
	const gchar *own[3] = { g_get_user_config_dir (), g_get_user_data_dir (), g_get_user_cache_dir () };
	const gchar *env[3] = { "HOST_XDG_CONFIG_HOME", "HOST_XDG_DATA_HOME", "HOST_XDG_CACHE_HOME" };
	const gchar *rel[3] = { ".config", ".local/share", ".cache" };

	if (!g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS))
		return base;

	for (guint i = 0; i < 3; i++) {
		if (!g_str_equal (base, own[i]))
			continue;
		if (!dirs[i])
			dirs[i] = g_getenv (env[i])?g_strdup (g_getenv (env[i])):g_build_filename (g_get_home_dir (), rel[i], NULL);
		return dirs[i];
	}

	return base;
}

static gchar *
install_import_source (const gchar *base, const gchar *filename)
{
	return g_build_filename (install_import_host_dir (base), UPSTREAM_DIR_NAME, filename, NULL);
}

static gchar *
install_import_target (const gchar *base, const gchar *filename)
{
	return g_build_filename (base, USER_DIR_NAME, filename, NULL);
}

/* TRUE if a process called "liferea" (upstream, not this fork) runs */
static gboolean
install_import_upstream_running (void)
{
	g_autoptr(GDir)	proc = g_dir_open ("/proc", 0, NULL);
	const gchar	*entry;

	while (proc && (entry = g_dir_read_name (proc))) {
		g_autofree gchar *path = NULL, *comm = NULL;

		if (!g_ascii_isdigit (entry[0]))
			continue;
		path = g_build_filename ("/proc", entry, "comm", NULL);
		if (g_file_get_contents (path, &comm, NULL, NULL) && g_str_equal (g_strstrip (comm), UPSTREAM_DIR_NAME))
			return TRUE;
	}

	return FALSE;
}

gchar *
install_import_describe_source (void)
{
	g_autofree gchar	*feedlist = install_import_source (g_get_user_config_dir (), "feedlist.opml");
	g_autofree gchar	*db = install_import_source (g_get_user_data_dir (), "liferea.db");
	g_autofree gchar	*opml = NULL;
	GString			*text;
	GStatBuf		st;
	guint			feeds = 0;

	if (!g_file_test (feedlist, G_FILE_TEST_EXISTS) && !g_file_test (db, G_FILE_TEST_EXISTS))
		return NULL;

	if (g_file_get_contents (feedlist, &opml, NULL, NULL))
		for (const gchar *p = opml; (p = strstr (p, "xmlUrl=")); p++)
			feeds++;

	text = g_string_new (NULL);
	g_string_append_printf (text, ngettext ("%u subscription", "%u subscriptions", feeds), feeds);
	if (0 == g_stat (db, &st)) {
		g_autofree gchar *size = g_format_size (st.st_size);
		g_string_append_printf (text, _(", a database of %s"), size);
	}
	g_string_append_printf (text, _(" (in %s)"), feedlist);
	if (install_import_upstream_running ())
		g_string_append (text, _("\n\nThe other Liferea is running. Its newest changes may be missing, close it first for a complete copy."));

	return g_string_free (text, FALSE);
}

gboolean
install_import_schedule (void)
{
	g_autofree gchar *marker = common_create_config_filename (IMPORT_MARKER);

	return g_file_set_contents (marker, "", 0, NULL);
}

/* Moves a file or folder of this fork into the backup folder */
static void
install_import_backup (const gchar *path, const gchar *backupDir)
{
	g_autofree gchar *name = g_path_get_basename (path);
	g_autofree gchar *target = g_build_filename (backupDir, name, NULL);

	if (!g_file_test (path, G_FILE_TEST_EXISTS))
		return;

	if (0 != g_rename (path, target))
		g_warning ("Import: could not back up %s to %s", path, target);
}

static void
install_import_copy_file (const gchar *from, const gchar *to)
{
	g_autoptr(GFile)	src = g_file_new_for_path (from);
	g_autoptr(GFile)	dst = g_file_new_for_path (to);
	g_autoptr(GError)	error = NULL;

	if (!g_file_test (from, G_FILE_TEST_EXISTS))
		return;

	if (!g_file_copy (src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error))
		g_warning ("Import: could not copy %s: %s", from, error->message);
}

static void
install_import_copy_dir (const gchar *from, const gchar *to)
{
	g_autoptr(GDir)	dir = g_dir_open (from, 0, NULL);
	const gchar	*name;

	if (!dir)
		return;

	g_mkdir_with_parents (to, 0700);
	while ((name = g_dir_read_name (dir))) {
		g_autofree gchar *src = g_build_filename (from, name, NULL);
		g_autofree gchar *dst = g_build_filename (to, name, NULL);

		if (g_file_test (src, G_FILE_TEST_IS_REGULAR))
			install_import_copy_file (src, dst);
	}
}

/* SQLite's online backup copies a consistent state, including changes
   still in the write-ahead log, even if the other Liferea is running */
static gboolean
install_import_copy_db (const gchar *from, const gchar *to)
{
	sqlite3		*src = NULL, *dst = NULL;
	sqlite3_backup	*backup;
	gint		res = SQLITE_ERROR;

	if (SQLITE_OK == sqlite3_open_v2 (from, &src, SQLITE_OPEN_READONLY, NULL) &&
	    SQLITE_OK == sqlite3_open (to, &dst)) {
		backup = sqlite3_backup_init (dst, "main", src, "main");
		if (backup) {
			sqlite3_backup_step (backup, -1);
			sqlite3_backup_finish (backup);
		}
		res = sqlite3_errcode (dst);
	}
	if (SQLITE_OK != res)
		g_warning ("Import: could not copy the database %s: %s", from, dst?sqlite3_errmsg (dst):"cannot open");

	sqlite3_close (src);
	sqlite3_close (dst);

	return SQLITE_OK == res;
}

gboolean
install_import_run_files (void)
{
	g_autofree gchar	*marker = NULL, *backupDir = NULL, *stamp = NULL;
	g_autoptr(GDateTime)	now = g_date_time_new_now_local ();
	const gchar		*configFiles[] = { "feedlist.opml", "liferea.css", NULL };
	const gchar		*dbFiles[] = { "liferea.db", "liferea.db-wal", "liferea.db-shm", NULL };

	marker = common_create_config_filename (IMPORT_MARKER);
	if (!g_file_test (marker, G_FILE_TEST_EXISTS))
		return FALSE;

	/* never repeat a failed import on every start */
	g_unlink (marker);

	/* 1. keep this fork's files in a backup folder */
	stamp = g_date_time_format (now, "before-import-%Y%m%d-%H%M%S");
	backupDir = install_import_target (g_get_user_data_dir (), stamp);
	g_mkdir_with_parents (backupDir, 0700);
	debug (DEBUG_CONF, "Import: backing up current data to %s", backupDir);

	for (guint i = 0; configFiles[i]; i++) {
		g_autofree gchar *path = install_import_target (g_get_user_config_dir (), configFiles[i]);
		install_import_backup (path, backupDir);
	}
	for (guint i = 0; dbFiles[i]; i++) {
		g_autofree gchar *path = install_import_target (g_get_user_data_dir (), dbFiles[i]);
		install_import_backup (path, backupDir);
	}
	{
		g_autofree gchar *favicons = install_import_target (g_get_user_cache_dir (), "favicons");
		install_import_backup (favicons, backupDir);
	}
	{
		/* entry filters only exist in this fork: keep ours unless the
		   other installation is a copy of this fork with its own */
		g_autofree gchar *from = install_import_source (g_get_user_config_dir (), "entry_filters.json");
		g_autofree gchar *to = install_import_target (g_get_user_config_dir (), "entry_filters.json");
		if (g_file_test (from, G_FILE_TEST_EXISTS)) {
			install_import_backup (to, backupDir);
			install_import_copy_file (from, to);
		}
	}

	/* 2. copy the other installation's files */
	for (guint i = 0; configFiles[i]; i++) {
		g_autofree gchar *from = install_import_source (g_get_user_config_dir (), configFiles[i]);
		g_autofree gchar *to = install_import_target (g_get_user_config_dir (), configFiles[i]);
		install_import_copy_file (from, to);
	}
	{
		g_autofree gchar *from = install_import_source (g_get_user_data_dir (), "liferea.db");
		g_autofree gchar *to = install_import_target (g_get_user_data_dir (), "liferea.db");
		if (g_file_test (from, G_FILE_TEST_EXISTS))
			install_import_copy_db (from, to);
	}
	{
		g_autofree gchar *from = install_import_source (g_get_user_cache_dir (), "favicons");
		g_autofree gchar *to = install_import_target (g_get_user_cache_dir (), "favicons");
		install_import_copy_dir (from, to);
	}

	g_message ("Imported the feed list and database of Liferea, previous data kept in %s", backupDir);
	importedFiles = TRUE;
	return TRUE;
}

/* Copies all keys existing with the same type in both schemas */
static void
install_import_copy_settings (const gchar *fromId, const gchar *toId)
{
	GSettingsSchemaSource		*source = g_settings_schema_source_get_default ();
	g_autoptr(GSettingsSchema)	fromSchema = NULL, toSchema = NULL;
	g_autoptr(GSettings)		from = NULL, to = NULL;
	g_auto(GStrv)			keys = NULL;

	if (!source)
		return;

	fromSchema = g_settings_schema_source_lookup (source, fromId, TRUE);
	toSchema = g_settings_schema_source_lookup (source, toId, TRUE);
	if (!fromSchema || !toSchema)
		return;

	from = g_settings_new_full (fromSchema, NULL, NULL);
	to = g_settings_new_full (toSchema, NULL, NULL);

	keys = g_settings_schema_list_keys (toSchema);
	for (guint i = 0; keys[i]; i++) {
		g_autoptr(GSettingsSchemaKey)	toKey = NULL;
		g_autoptr(GVariant)		value = NULL;

		if (!g_settings_schema_has_key (fromSchema, keys[i]))
			continue;

		/* only values the user changed, the defaults may differ */
		value = g_settings_get_user_value (from, keys[i]);
		toKey = g_settings_schema_get_key (toSchema, keys[i]);
		if (value && g_variant_is_of_type (value, g_settings_schema_key_get_value_type (toKey)) &&
		    g_settings_schema_key_range_check (toKey, value))
			g_settings_set_value (to, keys[i], value);
	}
	g_settings_sync ();
}

void
install_import_run_settings (void)
{
	if (!importedFiles)
		return;

	install_import_copy_settings (UPSTREAM_SCHEMA_ID, SCHEMA_ID);
	install_import_copy_settings (UPSTREAM_SCHEMA_ID ".plugins", SCHEMA_ID ".plugins");
}

void
install_import_restart (void)
{
	g_autofree gchar	*exe = g_file_read_link ("/proc/self/exe", NULL);
	g_autofree gchar	*script = NULL;
	g_autoptr(GError)	error = NULL;

	if (!exe)
		return;

	/* start again once this instance has quit (single instance app) */
	script = g_strdup_printf ("while kill -0 %d 2>/dev/null; do sleep 0.2; done; exec \"%s\"", (gint)getpid (), exe);
	{
		gchar *argv[] = { "/bin/sh", "-c", script, NULL };
		if (!g_spawn_async (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
			g_warning ("Could not restart: %s", error->message);
	}
}
