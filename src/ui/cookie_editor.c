/**
 * @file cookie_editor.c  looking at and changing kept cookies
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

#include "ui/cookie_editor.h"

#include <libadwaita-1/adwaita.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <webkit/webkit.h>

#include "common.h"
#include "debug.h"
#include "net.h"
#include "ui/liferea_shell.h"
#include "webkit/page_browser.h"

/* The two cookie stores: the browser loading's session (page feeds
   loaded with the built-in browser, site checks, Open Page) and the jar
   of feeds with "Keep cookies" loaded without browser. */

typedef struct {
	SoupCookie	*cookie;
	gboolean	browser;	/* in the browser loading's store */
} cookieEntry;

typedef struct {
	GtkWindow	*win;
	GtkListBox	*list;
	GtkWidget	*search;
	GtkWidget	*status;
	GPtrArray	*cookies;	/* cookieEntry */
} cookieEditor;

/* the cookie being changed (old NULL for a new one) */
typedef struct {
	cookieEditor	*editor;
	AdwDialog	*dialog;
	cookieEntry	*old;
	GtkWidget	*domain, *name, *value, *path, *expires, *store;
} cookieEdit;

static void cookie_editor_reload (cookieEditor *editor);

static void
cookie_entry_free (gpointer data)
{
	cookieEntry *entry = (cookieEntry *)data;

	soup_cookie_free (entry->cookie);
	g_free (entry);
}

static void
cookie_editor_free (gpointer data)
{
	cookieEditor *editor = (cookieEditor *)data;

	g_ptr_array_unref (editor->cookies);
	g_free (editor);
}

/* "2099-12-31" or "2099-12-31 18:30", NULL for cookies that go when the program quits */
static gchar *
cookie_expires_text (SoupCookie *cookie)
{
	GDateTime *expires = soup_cookie_get_expires (cookie);
	g_autoptr(GDateTime) local = NULL;

	if (!expires)
		return NULL;

	local = g_date_time_to_local (expires);
	if (g_date_time_get_hour (local) == 0 && g_date_time_get_minute (local) == 0)
		return g_date_time_format (local, "%Y-%m-%d");
	return g_date_time_format (local, "%Y-%m-%d %H:%M");
}

/* Reads "YYYY-MM-DD" with an optional "HH:MM"; empty text gives NULL
   (the cookie goes when the program quits). Returns FALSE if not understood. */
static gboolean
cookie_parse_expires (const gchar *text, GDateTime **result)
{
	g_autofree gchar	*trimmed = g_strstrip (g_strdup (text?text:""));
	gint			y, m, d, hh = 0, mm = 0, n;

	*result = NULL;
	if (!*trimmed)
		return TRUE;

	n = sscanf (trimmed, "%d-%d-%d %d:%d", &y, &m, &d, &hh, &mm);
	if (n != 3 && n != 5)
		return FALSE;

	*result = g_date_time_new_local (y, m, d, hh, mm, 0);
	return *result != NULL;
}

static gint
cookie_entry_compare (gconstpointer a, gconstpointer b)
{
	SoupCookie	*ca = (*(cookieEntry **)a)->cookie, *cb = (*(cookieEntry **)b)->cookie;
	const gchar	*da = soup_cookie_get_domain (ca), *db = soup_cookie_get_domain (cb);
	gint		result;

	/* ".example.com" next to "www.example.com" */
	if (*da == '.') da++;
	if (*db == '.') db++;
	result = g_ascii_strcasecmp (da, db);

	return result?result:g_strcmp0 (soup_cookie_get_name (ca), soup_cookie_get_name (cb));
}

/* editing one cookie */

static void
cookie_edit_free (gpointer data)
{
	g_free (data);
}

static void
on_browser_cookie_added (GObject *object, GAsyncResult *res, gpointer user_data)
{
	GtkWindow		*win = GTK_WINDOW (user_data);
	g_autoptr(GError)	error = NULL;
	cookieEditor		*editor = g_object_get_data (G_OBJECT (win), "editor");

	if (!webkit_cookie_manager_add_cookie_finish (WEBKIT_COOKIE_MANAGER (object), res, &error))
		debug (DEBUG_GUI, "cookie editor: adding failed: %s", error->message);
	if (editor)
		cookie_editor_reload (editor);
	g_object_unref (win);
}

typedef struct {
	GtkWindow	*win;
	SoupCookie	*add;	/* to add once the old one is gone (or NULL) */
} cookieReplace;

static void
on_browser_cookie_deleted (GObject *object, GAsyncResult *res, gpointer user_data)
{
	cookieReplace		*replace = (cookieReplace *)user_data;
	g_autoptr(GError)	error = NULL;
	cookieEditor		*editor = g_object_get_data (G_OBJECT (replace->win), "editor");

	if (!webkit_cookie_manager_delete_cookie_finish (WEBKIT_COOKIE_MANAGER (object), res, &error))
		debug (DEBUG_GUI, "cookie editor: deleting failed: %s", error->message);

	if (replace->add) {
		webkit_cookie_manager_add_cookie (WEBKIT_COOKIE_MANAGER (object), replace->add, NULL,
		                                  on_browser_cookie_added, g_object_ref (replace->win));
		soup_cookie_free (replace->add);
	} else if (editor) {
		cookie_editor_reload (editor);
	}

	g_object_unref (replace->win);
	g_free (replace);
}

/* puts "add" in place of "old" (either can be NULL) in the given store */
static void
cookie_editor_store (cookieEditor *editor, gboolean browser, SoupCookie *old, SoupCookie *add)
{
	if (browser) {
		WebKitCookieManager *manager = page_browser_get_cookie_manager ();

		if (old) {
			cookieReplace *replace = g_new0 (cookieReplace, 1);

			replace->win = g_object_ref (editor->win);
			replace->add = add?soup_cookie_copy (add):NULL;
			webkit_cookie_manager_delete_cookie (manager, old, NULL, on_browser_cookie_deleted, replace);
		} else if (add) {
			webkit_cookie_manager_add_cookie (manager, add, NULL, on_browser_cookie_added, g_object_ref (editor->win));
		}
	} else {
		SoupCookieJar *jar = network_get_kept_cookie_jar ();

		if (old)
			soup_cookie_jar_delete_cookie (jar, old);
		if (add)
			soup_cookie_jar_add_cookie (jar, soup_cookie_copy (add));
		cookie_editor_reload (editor);
	}
}

static void
on_edit_until_2099 (GtkButton *button, gpointer user_data)
{
	cookieEdit *edit = (cookieEdit *)user_data;

	gtk_editable_set_text (GTK_EDITABLE (edit->expires), "2099-12-31");
}

static void
on_edit_save (GtkButton *button, gpointer user_data)
{
	cookieEdit		*edit = (cookieEdit *)user_data;
	const gchar		*domain = gtk_editable_get_text (GTK_EDITABLE (edit->domain));
	const gchar		*name = gtk_editable_get_text (GTK_EDITABLE (edit->name));
	const gchar		*path = gtk_editable_get_text (GTK_EDITABLE (edit->path));
	g_autoptr(GDateTime)	expires = NULL;
	SoupCookie		*cookie;
	gboolean		browser;

	if (!*domain || !*name) {
		gtk_widget_add_css_class (*domain?edit->name:edit->domain, "error");
		return;
	}
	if (!cookie_parse_expires (gtk_editable_get_text (GTK_EDITABLE (edit->expires)), &expires)) {
		gtk_widget_add_css_class (edit->expires, "error");
		return;
	}

	/* a copy keeps the flags the site set (secure, same-site...) */
	if (edit->old) {
		cookie = soup_cookie_copy (edit->old->cookie);
		soup_cookie_set_domain (cookie, domain);
		soup_cookie_set_name (cookie, name);
		browser = edit->old->browser;
	} else {
		cookie = soup_cookie_new (name, "", domain, "/", -1);
		browser = adw_combo_row_get_selected (ADW_COMBO_ROW (edit->store)) == 0;
	}
	soup_cookie_set_value (cookie, gtk_editable_get_text (GTK_EDITABLE (edit->value)));
	soup_cookie_set_path (cookie, *path?path:"/");
	soup_cookie_set_expires (cookie, expires);

	debug (DEBUG_GUI, "cookie editor: saving %s on %s", name, domain);
	cookie_editor_store (edit->editor, browser, edit->old?edit->old->cookie:NULL, cookie);
	soup_cookie_free (cookie);
	adw_dialog_close (edit->dialog);
}

static void
on_edit_delete (GtkButton *button, gpointer user_data)
{
	cookieEdit *edit = (cookieEdit *)user_data;

	cookie_editor_store (edit->editor, edit->old->browser, edit->old->cookie, NULL);
	adw_dialog_close (edit->dialog);
}

static GtkWidget *
cookie_edit_row (GtkWidget *group, const gchar *title, const gchar *text, const gchar *tooltip)
{
	GtkWidget *row = adw_entry_row_new ();

	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
	gtk_editable_set_text (GTK_EDITABLE (row), text?text:"");
	if (tooltip)
		gtk_widget_set_tooltip_text (row, tooltip);
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);

	return row;
}

static void
cookie_editor_edit (cookieEditor *editor, cookieEntry *old)
{
	cookieEdit		*edit = g_new0 (cookieEdit, 1);
	GtkWidget		*toolbar = adw_toolbar_view_new ();
	GtkWidget		*page = adw_preferences_page_new ();
	GtkWidget		*group = adw_preferences_group_new ();
	GtkWidget		*buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget		*button;
	g_autofree gchar	*expires = old?cookie_expires_text (old->cookie):NULL;

	edit->editor = editor;
	edit->old = old;
	edit->dialog = adw_dialog_new ();
	adw_dialog_set_title (edit->dialog, old?_("Change Cookie"):_("Add Cookie"));
	adw_dialog_set_content_width (edit->dialog, 520);
	g_object_set_data_full (G_OBJECT (edit->dialog), "edit", edit, cookie_edit_free);

	adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());

	edit->domain = cookie_edit_row (group, _("Site"), old?soup_cookie_get_domain (old->cookie):NULL,
	                                _("A dot in front (\".example.com\") also covers the site's sub-sites (www. ...)"));
	edit->name = cookie_edit_row (group, _("Name"), old?soup_cookie_get_name (old->cookie):NULL, NULL);
	edit->value = cookie_edit_row (group, _("Value"), old?soup_cookie_get_value (old->cookie):NULL, NULL);
	edit->path = cookie_edit_row (group, _("Path"), old?soup_cookie_get_path (old->cookie):"/", NULL);
	edit->expires = cookie_edit_row (group, _("Keep until (YYYY-MM-DD)"), expires,
	                                 _("Year-month-day, optionally with a time (2099-12-31 18:00). Empty: the cookie goes when Liferea-Wonder quits."));

	button = gtk_button_new_with_mnemonic (_("Until _2099"));
	gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text (button, _("Keep the cookie (e.g. an age check) so the site does not ask again"));
	g_signal_connect (button, "clicked", G_CALLBACK (on_edit_until_2099), edit);
	adw_entry_row_add_suffix (ADW_ENTRY_ROW (edit->expires), button);

	if (!old) {
		const gchar *stores[] = { N_("Browser loading (page feeds loaded with the browser, site checks)"),
		                          N_("Feeds with \"Keep cookies\" (loaded without browser)"), NULL };
		g_autoptr(GtkStringList) list = gtk_string_list_new (NULL);

		for (guint i = 0; stores[i]; i++)
			gtk_string_list_append (list, _(stores[i]));
		edit->store = adw_combo_row_new ();
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (edit->store), _("Used by"));
		adw_combo_row_set_model (ADW_COMBO_ROW (edit->store), G_LIST_MODEL (list));
		adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), edit->store);
	}

	adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

	gtk_widget_set_margin_start (buttons, 12);
	gtk_widget_set_margin_end (buttons, 12);
	gtk_widget_set_margin_bottom (buttons, 12);
	gtk_widget_set_halign (buttons, GTK_ALIGN_END);
	if (old) {
		button = gtk_button_new_with_mnemonic (_("_Delete"));
		gtk_widget_add_css_class (button, "destructive-action");
		g_signal_connect (button, "clicked", G_CALLBACK (on_edit_delete), edit);
		gtk_box_append (GTK_BOX (buttons), button);
	}
	button = gtk_button_new_with_mnemonic (_("_Save"));
	gtk_widget_add_css_class (button, "suggested-action");
	g_signal_connect (button, "clicked", G_CALLBACK (on_edit_save), edit);
	gtk_box_append (GTK_BOX (buttons), button);

	adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);
	adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), buttons);
	adw_dialog_set_child (edit->dialog, toolbar);
	adw_dialog_present (edit->dialog, GTK_WIDGET (editor->win));
}

/* the list */

static void
on_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
	cookieEditor	*editor = (cookieEditor *)user_data;
	cookieEntry	*entry = g_object_get_data (G_OBJECT (row), "entry");

	if (entry)
		cookie_editor_edit (editor, entry);
}

static void
on_add_clicked (GtkButton *button, gpointer user_data)
{
	cookie_editor_edit ((cookieEditor *)user_data, NULL);
}

static gboolean
cookie_editor_filter (GtkListBoxRow *row, gpointer user_data)
{
	cookieEditor	*editor = (cookieEditor *)user_data;
	cookieEntry	*entry = g_object_get_data (G_OBJECT (row), "entry");
	const gchar	*text = gtk_editable_get_text (GTK_EDITABLE (editor->search));
	g_autofree gchar *needle = NULL, *hay = NULL;

	if (!entry || !text || !*text)
		return TRUE;

	needle = g_utf8_casefold (text, -1);
	hay = g_utf8_casefold (soup_cookie_get_domain (entry->cookie), -1);
	if (strstr (hay, needle))
		return TRUE;
	g_free (hay);
	hay = g_utf8_casefold (soup_cookie_get_name (entry->cookie), -1);

	return strstr (hay, needle) != NULL;
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
	gtk_list_box_invalidate_filter (((cookieEditor *)user_data)->list);
}

static void
cookie_editor_fill (cookieEditor *editor)
{
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child (GTK_WIDGET (editor->list))))
		gtk_list_box_remove (editor->list, child);

	g_ptr_array_sort (editor->cookies, cookie_entry_compare);
	for (guint i = 0; i < editor->cookies->len; i++) {
		cookieEntry		*entry = g_ptr_array_index (editor->cookies, i);
		GtkWidget		*row = adw_action_row_new ();
		g_autofree gchar	*expires = cookie_expires_text (entry->cookie);
		g_autofree gchar	*title = g_markup_printf_escaped ("%s  <b>%s</b>", soup_cookie_get_domain (entry->cookie), soup_cookie_get_name (entry->cookie));
		g_autofree gchar	*value = g_utf8_substring (soup_cookie_get_value (entry->cookie), 0,
		                                                   MIN (60, g_utf8_strlen (soup_cookie_get_value (entry->cookie), -1)));
		g_autofree gchar	*subtitle = NULL;
		g_autofree gchar	*until = expires?g_strdup_printf (_("kept until %s"), expires):g_strdup (_("until Liferea-Wonder quits"));

		subtitle = g_markup_printf_escaped ("%s = %s%s · %s · %s",
		                                    soup_cookie_get_name (entry->cookie), value,
		                                    strlen (soup_cookie_get_value (entry->cookie)) > strlen (value)?"…":"",
		                                    until, entry->browser?_("browser loading"):_("kept cookies"));
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
		adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
		adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
		adw_action_row_add_suffix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("go-next-symbolic"));
		gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
		g_object_set_data (G_OBJECT (row), "entry", entry);
		gtk_list_box_append (editor->list, row);
	}

	gtk_label_set_text (GTK_LABEL (editor->status),
	                    editor->cookies->len?"":_("No cookies yet. Sites set them when feeds load (or add one with +)."));
}

static void
on_browser_cookies (GObject *object, GAsyncResult *res, gpointer user_data)
{
	GtkWindow		*win = GTK_WINDOW (user_data);
	cookieEditor		*editor = g_object_get_data (G_OBJECT (win), "editor");
	g_autoptr(GError)	error = NULL;
	GList			*cookies = webkit_cookie_manager_get_all_cookies_finish (WEBKIT_COOKIE_MANAGER (object), res, &error);

	if (editor) {
		for (GList *iter = cookies; iter; iter = iter->next) {
			cookieEntry *entry = g_new0 (cookieEntry, 1);

			entry->cookie = soup_cookie_copy ((SoupCookie *)iter->data);
			entry->browser = TRUE;
			g_ptr_array_add (editor->cookies, entry);
		}
		if (error)
			debug (DEBUG_GUI, "cookie editor: browser cookies not readable: %s", error->message);
		cookie_editor_fill (editor);
	}

	g_list_free_full (cookies, (GDestroyNotify)soup_cookie_free);
	g_object_unref (win);
}

static void
cookie_editor_reload (cookieEditor *editor)
{
	GSList *kept = soup_cookie_jar_all_cookies (network_get_kept_cookie_jar ());

	g_ptr_array_set_size (editor->cookies, 0);
	for (GSList *iter = kept; iter; iter = iter->next) {
		cookieEntry *entry = g_new0 (cookieEntry, 1);

		entry->cookie = (SoupCookie *)iter->data;	/* copies, now ours */
		g_ptr_array_add (editor->cookies, entry);
	}
	g_slist_free (kept);

	webkit_cookie_manager_get_all_cookies (page_browser_get_cookie_manager (), NULL,
	                                       on_browser_cookies, g_object_ref (editor->win));
}

static void
on_editor_destroy (GtkWidget *win, gpointer user_data)
{
	/* callbacks still on their way find no editor */
	g_object_set_data (G_OBJECT (win), "editor", NULL);
}

void
cookie_editor_open (const gchar *filter)
{
	cookieEditor	*editor = g_new0 (cookieEditor, 1);
	GtkWidget	*win = adw_window_new ();
	GtkWidget	*toolbar = adw_toolbar_view_new ();
	GtkWidget	*header = adw_header_bar_new ();
	GtkWidget	*box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget	*scrolled = gtk_scrolled_window_new ();
	GtkWidget	*clamp = adw_clamp_new ();
	GtkWidget	*add = gtk_button_new_from_icon_name ("list-add-symbolic");
	GtkWidget	*info;

	editor->win = GTK_WINDOW (win);
	editor->cookies = g_ptr_array_new_with_free_func (cookie_entry_free);
	g_object_set_data_full (G_OBJECT (win), "editor", editor, cookie_editor_free);
	g_signal_connect (win, "destroy", G_CALLBACK (on_editor_destroy), NULL);

	gtk_widget_set_tooltip_text (add, _("Add a cookie"));
	g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), editor);
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), add);
	adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

	gtk_widget_set_margin_top (box, 12);
	gtk_widget_set_margin_bottom (box, 12);
	gtk_widget_set_margin_start (box, 12);
	gtk_widget_set_margin_end (box, 12);

	info = gtk_label_new (_("Cookies sites gave to Liferea-Wonder. Click one to change its value or how long it is kept, e.g. keep an age check cookie until 2099 so the site stops asking."));
	gtk_label_set_wrap (GTK_LABEL (info), TRUE);
	gtk_label_set_xalign (GTK_LABEL (info), 0);
	gtk_widget_add_css_class (info, "dim-label");
	gtk_box_append (GTK_BOX (box), info);

	editor->search = gtk_search_entry_new ();
	gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (editor->search), _("Site or cookie name"));
	if (filter)
		gtk_editable_set_text (GTK_EDITABLE (editor->search), filter);
	g_signal_connect (editor->search, "search-changed", G_CALLBACK (on_search_changed), editor);
	gtk_box_append (GTK_BOX (box), editor->search);

	editor->list = GTK_LIST_BOX (gtk_list_box_new ());
	gtk_list_box_set_selection_mode (editor->list, GTK_SELECTION_NONE);
	gtk_widget_add_css_class (GTK_WIDGET (editor->list), "boxed-list");
	gtk_widget_set_valign (GTK_WIDGET (editor->list), GTK_ALIGN_START);
	gtk_list_box_set_filter_func (editor->list, cookie_editor_filter, editor, NULL);
	g_signal_connect (editor->list, "row-activated", G_CALLBACK (on_row_activated), editor);
	gtk_box_append (GTK_BOX (box), GTK_WIDGET (editor->list));

	editor->status = gtk_label_new (NULL);
	gtk_widget_add_css_class (editor->status, "dim-label");
	gtk_box_append (GTK_BOX (box), editor->status);

	adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 800);
	adw_clamp_set_child (ADW_CLAMP (clamp), box);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
	gtk_widget_set_vexpand (scrolled, TRUE);
	adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), scrolled);
	adw_window_set_content (ADW_WINDOW (win), toolbar);

	gtk_window_set_title (GTK_WINDOW (win), _("Cookies"));
	gtk_window_set_default_size (GTK_WINDOW (win), 760, 640);
	gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (liferea_shell_get_window ()));
	/* own group, see page_browser.c: modal dialogs must not lock it */
	{
		GtkWindowGroup *group = gtk_window_group_new ();
		gtk_window_group_add_window (group, GTK_WINDOW (win));
		g_object_unref (group);
	}

	cookie_editor_reload (editor);
	gtk_window_present (GTK_WINDOW (win));
}
