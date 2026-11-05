/**
 * @file entry_filter_dialog.c  entry filter management dialogs
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

#include "ui/entry_filter_dialog.h"

#include <libadwaita-1/adwaita.h>

#include "common.h"
#include "entry_filter.h"
#include "feedlist.h"
#include "node.h"
#include "node_provider.h"
#include "ui/liferea_dialog.h"

/* The manager dialog works on a copy of all filters ("filters" data
   of the dialog) which replaces the active filters on "Save". */

static const gchar * match_options[] = {
	N_("Whole word"),
	N_("Phrase (text anywhere)"),
	N_("Regular expression"),
	NULL
};

static const gchar * action_options[] = {
	N_("Exclude matching entries"),
	N_("Only include matching entries"),
	N_("Mark matching entries as read"),
	N_("Flag matching entries"),
	NULL
};

static const struct {
	const gchar		*widget;
	entryFilterField	field;
	const gchar		*label;
} field_widgets[] = {
	{ "fieldTitle",		ENTRY_FILTER_FIELD_TITLE,	N_("title") },
	{ "fieldContent",	ENTRY_FILTER_FIELD_CONTENT,	N_("content") },
	{ "fieldAuthor",	ENTRY_FILTER_FIELD_AUTHOR,	N_("author") },
	{ "fieldCategory",	ENTRY_FILTER_FIELD_CATEGORY,	N_("categories") },
	{ "fieldLink",		ENTRY_FILTER_FIELD_LINK,	N_("link") }
};

static void entry_filter_dialog_refresh (GtkWidget *dialog);

static void
entry_filter_dialog_setup_drop_down (GtkWidget *dialog, const gchar *name, const gchar **options, guint selected)
{
	GtkDropDown		*dropdown = GTK_DROP_DOWN (liferea_dialog_lookup (dialog, name));
	g_autoptr(GtkStringList) list = gtk_string_list_new (NULL);

	for (; *options; options++)
		gtk_string_list_append (list, _(*options));
	gtk_drop_down_set_model (dropdown, G_LIST_MODEL (list));
	gtk_drop_down_set_selected (dropdown, selected);
}

/* Plain words summary like "Exclude · word "foo" in title, content · 2 feeds/folders" */
static gchar *
entry_filter_dialog_describe (entryFilterPtr filter)
{
	GString		*fields = g_string_new (NULL);
	g_autofree gchar *targets = NULL;
	gchar		*summary;
	const gchar	*action, *match;

	for (guint i = 0; i < G_N_ELEMENTS (field_widgets); i++) {
		if (filter->fields & field_widgets[i].field) {
			if (fields->len)
				g_string_append (fields, ", ");
			g_string_append (fields, _(field_widgets[i].label));
		}
	}

	switch (filter->action) {
		case ENTRY_FILTER_ACTION_INCLUDE_ONLY:	action = _("Only include"); break;
		case ENTRY_FILTER_ACTION_MARK_READ:	action = _("Mark read"); break;
		case ENTRY_FILTER_ACTION_FLAG:		action = _("Flag"); break;
		default:				action = _("Exclude"); break;
	}

	switch (filter->match) {
		case ENTRY_FILTER_MATCH_PHRASE:	match = _("phrase"); break;
		case ENTRY_FILTER_MATCH_REGEX:	match = _("regex"); break;
		default:			match = _("word"); break;
	}

	if (filter->allFeeds) {
		targets = g_strdup (_("all feeds"));
	} else {
		GString	*names = g_string_new (NULL);
		guint	count = g_slist_length (filter->nodeIds), shown = 0;

		for (GSList *iter = filter->nodeIds; iter && shown < 2; iter = g_slist_next (iter)) {
			Node *node = node_is_used_id ((const gchar *)iter->data);
			if (!node)
				continue;
			g_string_append_printf (names, "%s%s", shown?", ":"", node_get_title (node));
			shown++;
		}
		if (count > shown)
			g_string_append_printf (names, ngettext (" and %u more", " and %u more", count - shown), count - shown);
		targets = g_string_free (names, FALSE);
	}

	summary = g_strdup_printf ("%s · %s “%s” %s %s%s · %s",
	                           action, match, filter->pattern,
	                           _("in"), fields->len?fields->str:_("nothing"),
	                           filter->caseSensitive?_(" (case sensitive)"):"",
	                           targets);
	g_string_free (fields, TRUE);

	return summary;
}

/* editor dialog */

static void
entry_filter_edit_add_node_rows (GtkListBox *list, Node *parent, entryFilterPtr filter, guint depth)
{
	for (GSList *iter = parent->children; iter; iter = g_slist_next (iter)) {
		Node		*node = (Node *)iter->data;
		GtkWidget	*check;

		/* search folders and news bins are never updated */
		if (IS_VFOLDER (node) || IS_NEWSBIN (node))
			continue;

		check = gtk_check_button_new_with_label (node_get_title (node));
		gtk_widget_set_margin_start (check, 6 + 18 * depth);
		gtk_check_button_set_active (GTK_CHECK_BUTTON (check),
		                             NULL != g_slist_find_custom (filter->nodeIds, node->id, (GCompareFunc)g_strcmp0));
		g_object_set_data_full (G_OBJECT (check), "nodeId", g_strdup (node->id), g_free);
		gtk_list_box_append (list, check);

		if (node->children)
			entry_filter_edit_add_node_rows (list, node, filter, depth + 1);
	}
}

static void
on_entry_filter_edit_scope_toggled (GtkCheckButton *button, gpointer user_data)
{
	GtkWidget *dialog = GTK_WIDGET (user_data);

	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "nodeScroll"),
	                          gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "selectedFeedsRadio"))));
}

static void
on_entry_filter_edit_apply (GtkButton *button, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	GtkWidget		*manager = GTK_WIDGET (g_object_get_data (G_OBJECT (dialog), "manager"));
	entryFilterPtr		filter = (entryFilterPtr)g_object_get_data (G_OBJECT (dialog), "filter");
	GtkWidget		*errorLabel = liferea_dialog_lookup (dialog, "errorLabel");
	GtkWidget		*child;
	g_autoptr(GError)	error = NULL;
	GSList			*filters;

	g_free (filter->name);
	filter->name = g_strdup (liferea_dialog_entryrow_get (dialog, "nameEntry"));
	g_free (filter->pattern);
	filter->pattern = g_strdup (liferea_dialog_entryrow_get (dialog, "patternEntry"));
	filter->match = gtk_drop_down_get_selected (GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "matchDropDown")));
	filter->action = gtk_drop_down_get_selected (GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "actionDropDown")));
	filter->caseSensitive = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "caseCheck")));
	filter->enabled = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "enabledCheck")));
	filter->allFeeds = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "allFeedsRadio")));

	filter->fields = 0;
	for (guint i = 0; i < G_N_ELEMENTS (field_widgets); i++)
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, field_widgets[i].widget))))
			filter->fields |= field_widgets[i].field;

	g_slist_free_full (filter->nodeIds, g_free);
	filter->nodeIds = NULL;
	for (child = gtk_widget_get_first_child (liferea_dialog_lookup (dialog, "nodeList")); child; child = gtk_widget_get_next_sibling (child)) {
		GtkWidget *check = gtk_list_box_row_get_child (GTK_LIST_BOX_ROW (child));
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (check)))
			filter->nodeIds = g_slist_append (filter->nodeIds, g_strdup (g_object_get_data (G_OBJECT (check), "nodeId")));
	}

	/* validate before closing */
	if (!entry_filter_compile (filter, &error)) {
		g_autofree gchar *text = g_strdup_printf (_("The text to match is not valid: %s"), error->message);
		gtk_label_set_text (GTK_LABEL (errorLabel), text);
		gtk_widget_set_visible (errorLabel, TRUE);
		return;
	}
	if (!filter->fields) {
		gtk_label_set_text (GTK_LABEL (errorLabel), _("Choose at least one place to look in."));
		gtk_widget_set_visible (errorLabel, TRUE);
		return;
	}
	if (!filter->allFeeds && !filter->nodeIds) {
		gtk_label_set_text (GTK_LABEL (errorLabel), _("Check at least one feed or folder, or apply the filter to all feeds."));
		gtk_widget_set_visible (errorLabel, TRUE);
		return;
	}

	if (!*filter->name) {
		g_free (filter->name);
		filter->name = g_strdup (filter->pattern);
	}

	/* opened from the feed list: add to the active filters right away */
	if (!manager) {
		GSList *all = NULL;

		for (GSList *iter = entry_filter_get_all (); iter; iter = g_slist_next (iter))
			all = g_slist_append (all, entry_filter_copy ((entryFilterPtr)iter->data));
		entry_filter_set_all (g_slist_append (all, filter));
		g_object_set_data (G_OBJECT (dialog), "filter", NULL);	/* owned by entry_filter.c now */
		adw_dialog_close (ADW_DIALOG (dialog));
		return;
	}

	/* hand the edited copy over to the manager's working list */
	filters = (GSList *)g_object_get_data (G_OBJECT (manager), "filters");
	if (g_object_get_data (G_OBJECT (dialog), "original")) {
		GSList *link = g_slist_find (filters, g_object_get_data (G_OBJECT (dialog), "original"));
		entry_filter_free (link->data);
		link->data = filter;
	} else {
		filters = g_slist_append (filters, filter);
	}
	g_object_set_data (G_OBJECT (manager), "filters", filters);
	g_object_set_data (G_OBJECT (dialog), "filter", NULL);	/* owned by the list now */

	entry_filter_dialog_refresh (manager);
	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_entry_filter_edit_closed (GtkWidget *dialog, gpointer user_data)
{
	/* discard the copy when cancelled */
	entry_filter_free ((entryFilterPtr)g_object_get_data (G_OBJECT (dialog), "filter"));
}

/* Opens the editor for an existing filter (original) or a new one. Without
   a manager dialog (opened from the feed list) OK saves right away. */
static void
entry_filter_edit_open (GtkWidget *manager, entryFilterPtr original, Node *preset)
{
	GtkWidget	*dialog = liferea_dialog_new ("entry_filter_edit");
	entryFilterPtr	filter;

	if (!dialog)
		return;

	if (original) {
		filter = entry_filter_copy (original);
	} else {
		Node *target = preset?preset:feedlist_get_selected ();

		/* filters are meant for single feeds or folders: preset the
		   given or selected one, "all feeds" must be chosen on purpose */
		filter = entry_filter_new ();
		if (target && !IS_VFOLDER (target) && !IS_NEWSBIN (target) && target != feedlist_get_root ())
			filter->nodeIds = g_slist_append (filter->nodeIds, g_strdup (target->id));
		if (target && preset) {
			g_autofree gchar *title = g_strdup_printf (_("New Entry Filter for “%s”"), node_get_title (target));
			adw_dialog_set_title (ADW_DIALOG (dialog), title);
		} else {
			adw_dialog_set_title (ADW_DIALOG (dialog), _("New Entry Filter"));
		}
	}

	g_object_set_data (G_OBJECT (dialog), "manager", manager);
	g_object_set_data (G_OBJECT (dialog), "original", original);
	g_object_set_data (G_OBJECT (dialog), "filter", filter);

	liferea_dialog_entryrow_set (dialog, "nameEntry", filter->name);
	liferea_dialog_entryrow_set (dialog, "patternEntry", filter->pattern);
	entry_filter_dialog_setup_drop_down (dialog, "matchDropDown", match_options, filter->match);
	entry_filter_dialog_setup_drop_down (dialog, "actionDropDown", action_options, filter->action);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "caseCheck")), filter->caseSensitive);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "enabledCheck")), filter->enabled);
	for (guint i = 0; i < G_N_ELEMENTS (field_widgets); i++)
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, field_widgets[i].widget)), 0 != (filter->fields & field_widgets[i].field));

	entry_filter_edit_add_node_rows (GTK_LIST_BOX (liferea_dialog_lookup (dialog, "nodeList")), feedlist_get_root (), filter, 0);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, filter->allFeeds?"allFeedsRadio":"selectedFeedsRadio")), TRUE);
	g_signal_connect (liferea_dialog_lookup (dialog, "allFeedsRadio"), "toggled", G_CALLBACK (on_entry_filter_edit_scope_toggled), dialog);
	on_entry_filter_edit_scope_toggled (NULL, dialog);

	g_signal_connect (dialog, "closed", G_CALLBACK (on_entry_filter_edit_closed), NULL);
	g_signal_connect (liferea_dialog_lookup (dialog, "applyBtn"), "clicked", G_CALLBACK (on_entry_filter_edit_apply), dialog);
	g_signal_connect_swapped (liferea_dialog_lookup (dialog, "cancelBtn"), "clicked", G_CALLBACK (adw_dialog_close), dialog);
}

/* manager dialog */

static entryFilterPtr
entry_filter_dialog_get_selected (GtkWidget *dialog)
{
	GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (liferea_dialog_lookup (dialog, "filterList")));

	return row?(entryFilterPtr)g_object_get_data (G_OBJECT (row), "filter"):NULL;
}

static void
on_entry_filter_enabled_toggled (GtkCheckButton *button, gpointer user_data)
{
	((entryFilterPtr)user_data)->enabled = gtk_check_button_get_active (button);
}

static void
on_entry_filter_selection_changed (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
	GtkWidget *dialog = GTK_WIDGET (user_data);

	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "editBtn"), NULL != row);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "removeBtn"), NULL != row);
}

static void
entry_filter_dialog_refresh (GtkWidget *dialog)
{
	GtkListBox	*list = GTK_LIST_BOX (liferea_dialog_lookup (dialog, "filterList"));
	GSList		*filters = (GSList *)g_object_get_data (G_OBJECT (dialog), "filters");

	gtk_list_box_remove_all (list);

	for (GSList *iter = filters; iter; iter = g_slist_next (iter)) {
		entryFilterPtr		filter = (entryFilterPtr)iter->data;
		GtkWidget		*row = adw_action_row_new ();
		GtkWidget		*check = gtk_check_button_new ();
		g_autofree gchar	*summary = entry_filter_dialog_describe (filter);

		adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), filter->name);
		adw_action_row_set_subtitle (ADW_ACTION_ROW (row), summary);
		gtk_widget_set_tooltip_text (check, _("Enabled"));
		gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
		gtk_check_button_set_active (GTK_CHECK_BUTTON (check), filter->enabled);
		g_signal_connect (check, "toggled", G_CALLBACK (on_entry_filter_enabled_toggled), filter);
		adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
		g_object_set_data (G_OBJECT (row), "filter", filter);
		gtk_list_box_append (list, row);
	}

	if (!filters) {
		GtkWidget *row = adw_action_row_new ();
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("No filters yet. Use \"Add...\" to create one."));
		gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
		gtk_list_box_append (list, row);
	}

	on_entry_filter_selection_changed (list, NULL, dialog);
}

static void
on_entry_filter_add (GtkButton *button, gpointer user_data)
{
	entry_filter_edit_open (GTK_WIDGET (user_data), NULL, NULL);
}

static void
on_entry_filter_edit (GtkButton *button, gpointer user_data)
{
	entryFilterPtr filter = entry_filter_dialog_get_selected (GTK_WIDGET (user_data));

	if (filter)
		entry_filter_edit_open (GTK_WIDGET (user_data), filter, NULL);
}

static void
on_entry_filter_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
	entryFilterPtr filter = (entryFilterPtr)g_object_get_data (G_OBJECT (row), "filter");

	if (filter)
		entry_filter_edit_open (GTK_WIDGET (user_data), filter, NULL);
}

static void
on_entry_filter_remove (GtkButton *button, gpointer user_data)
{
	GtkWidget	*dialog = GTK_WIDGET (user_data);
	entryFilterPtr	filter = entry_filter_dialog_get_selected (dialog);
	GSList		*filters = (GSList *)g_object_get_data (G_OBJECT (dialog), "filters");

	if (!filter)
		return;

	filters = g_slist_remove (filters, filter);
	entry_filter_free (filter);
	g_object_set_data (G_OBJECT (dialog), "filters", filters);
	entry_filter_dialog_refresh (dialog);
}

static void
on_entry_filters_save (GtkButton *button, gpointer user_data)
{
	GtkWidget *dialog = GTK_WIDGET (user_data);

	entry_filter_set_all ((GSList *)g_object_get_data (G_OBJECT (dialog), "filters"));
	g_object_set_data (G_OBJECT (dialog), "filters", NULL);	/* owned by entry_filter.c now */
	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_entry_filters_closed (GtkWidget *dialog, gpointer user_data)
{
	g_slist_free_full ((GSList *)g_object_get_data (G_OBJECT (dialog), "filters"), (GDestroyNotify)entry_filter_free);
	g_object_set_data (G_OBJECT (dialog), "filters", NULL);
}

void
entry_filter_dialog_open (void)
{
	GtkWidget	*dialog = liferea_dialog_new ("entry_filters");
	GtkWidget	*list;
	GSList		*copy = NULL;

	if (!dialog)
		return;

	for (GSList *iter = entry_filter_get_all (); iter; iter = g_slist_next (iter))
		copy = g_slist_append (copy, entry_filter_copy ((entryFilterPtr)iter->data));
	g_object_set_data (G_OBJECT (dialog), "filters", copy);

	list = liferea_dialog_lookup (dialog, "filterList");
	g_signal_connect (list, "row-selected", G_CALLBACK (on_entry_filter_selection_changed), dialog);
	g_signal_connect (list, "row-activated", G_CALLBACK (on_entry_filter_row_activated), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "addBtn"), "clicked", G_CALLBACK (on_entry_filter_add), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "editBtn"), "clicked", G_CALLBACK (on_entry_filter_edit), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "removeBtn"), "clicked", G_CALLBACK (on_entry_filter_remove), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "applyBtn"), "clicked", G_CALLBACK (on_entry_filters_save), dialog);
	g_signal_connect_swapped (liferea_dialog_lookup (dialog, "cancelBtn"), "clicked", G_CALLBACK (adw_dialog_close), dialog);
	g_signal_connect (dialog, "closed", G_CALLBACK (on_entry_filters_closed), NULL);

	entry_filter_dialog_refresh (dialog);
}

void
entry_filter_dialog_new_for_node (Node *node)
{
	entry_filter_edit_open (NULL, NULL, node);
}
