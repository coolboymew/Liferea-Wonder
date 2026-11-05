/**
 * @file folder.c  sub folders for hierarchic node sources
 *
 * Copyright (C) 2006-2026 Lars Windolf <lars.windolf@gmx.de>
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

#include "node_providers/folder.h"

#include "common.h"
#include "conf.h"
#include "debug.h"
#include "feedlist.h"
#include "itemset.h"
#include "node.h"
#include "node_providers/vfolder.h"
#include "ui/feed_list_view.h"
#include "ui/icons.h"
#include "ui/liferea_dialog.h"

/* Note: The folder node type implements the behaviour of a folder like
   node in the feed list. The two most important features are viewing the
   unread items of all child feeds and displaying the aggregated unread count
   of all child feeds.

   The folder node type does not implement the hierarchy of the feed list! */

/* Folder update schedule, kept in node->data (NULL means defaults) */
typedef struct folderSettings {
	gint		updateInterval;		/*<< minutes, -1 inherit, -2 no auto update */
	gboolean	overrideChildren;	/*<< if TRUE the folder schedule wins over the children's */
} *folderSettingsPtr;

static folderSettingsPtr
folder_get_settings (Node *node, gboolean create)
{
	if (!node->data && create) {
		folderSettingsPtr settings = g_new0 (struct folderSettings, 1);
		settings->updateInterval = -1;
		node_set_data (node, settings);
	}

	return (folderSettingsPtr)node->data;
}

gint
folder_get_update_interval (Node *node)
{
	folderSettingsPtr settings = folder_get_settings (node, FALSE);

	return settings?settings->updateInterval:-1;
}

void
folder_set_update_interval (Node *node, gint interval)
{
	g_assert (interval >= -2);

	folder_get_settings (node, TRUE)->updateInterval = interval;
	feedlist_schedule_save ();
}

gboolean
folder_get_override_children (Node *node)
{
	folderSettingsPtr settings = folder_get_settings (node, FALSE);

	return settings?settings->overrideChildren:FALSE;
}

void
folder_set_override_children (Node *node, gboolean override)
{
	folder_get_settings (node, TRUE)->overrideChildren = override;
	feedlist_schedule_save ();
}

gint
folder_get_scheduled_interval (Node *node, gint ownInterval, Node **decidingFolder)
{
	GSList		*folders = NULL, *iter;
	Node		*parent, *inheritedFrom = NULL, *forcedBy = NULL;
	gint		inherited = -1, forced = -1, result;

	if (!node)
		return ownInterval;

	/* collect all parent folders, outermost first */
	for (parent = node->parent; parent; parent = parent->parent)
		if (IS_FOLDER (parent))
			folders = g_slist_prepend (folders, parent);

	for (iter = folders; iter; iter = g_slist_next (iter)) {
		Node *folder = (Node *)iter->data;
		gint interval = folder_get_update_interval (folder);

		if (-1 != interval) {
			inherited = interval;
			inheritedFrom = folder;
		}

		/* the outermost overriding folder wins, it passes on its
		   own schedule or the one it inherits itself */
		if (!forcedBy && folder_get_override_children (folder)) {
			forced = inherited;
			forcedBy = folder;
		}
	}
	g_slist_free (folders);

	if (forcedBy) {
		result = forced;
		parent = forcedBy;
	} else if (-1 != ownInterval) {
		result = ownInterval;
		parent = NULL;
	} else {
		result = inherited;
		parent = inheritedFrom;
	}

	if (decidingFolder)
		*decidingFolder = parent;

	return result;
}

static void
folder_merge_child_items (Node *node, gpointer user_data)
{
	itemSetPtr	folderItemSet = (itemSetPtr)user_data;
	itemSetPtr	nodeItemSet;

	nodeItemSet = node_get_itemset (node);
	folderItemSet->ids = g_list_concat (folderItemSet->ids, nodeItemSet->ids);
	nodeItemSet->ids = NULL;
	itemset_free (nodeItemSet);
}

static itemSetPtr
folder_load (Node *node)
{
	itemSetPtr	itemSet;

	itemSet = g_new0 (struct itemSet, 1);
	itemSet->nodeId = node->id;

	node_foreach_child_data (node, folder_merge_child_items, itemSet);
	return itemSet;
}

static void
folder_import (Node *node, Node *parent, xmlNodePtr cur, gboolean trusted)
{
	xmlChar	*tmp;

	tmp = xmlGetProp (cur, BAD_CAST"updateInterval");
	if (tmp) {
		gint interval = common_parse_long ((gchar *)tmp, -1);
		folder_get_settings (node, TRUE)->updateInterval = (interval < -2)?-1:interval;
		xmlFree (tmp);
	}

	tmp = xmlGetProp (cur, BAD_CAST"overrideChildIntervals");
	if (tmp && !xmlStrcmp (tmp, BAD_CAST"true"))
		folder_get_settings (node, TRUE)->overrideChildren = TRUE;
	xmlFree (tmp);
}

static void
folder_export (Node *node, xmlNodePtr cur, gboolean trusted)
{
	if (-1 != folder_get_update_interval (node)) {
		g_autofree gchar *interval = g_strdup_printf ("%d", folder_get_update_interval (node));
		xmlNewProp (cur, BAD_CAST"updateInterval", BAD_CAST interval);
	}

	if (folder_get_override_children (node))
		xmlNewProp (cur, BAD_CAST"overrideChildIntervals", BAD_CAST"true");
}

static void
folder_free (Node *node)
{
	g_clear_pointer (&node->data, g_free);
}

static void
folder_add_child_update_counters (Node *node, gpointer user_data)
{
	guint	*unreadCount = (guint *)user_data;

	if (!IS_VFOLDER (node))
		*unreadCount += node->unreadCount;
}

static void
folder_update_counters (Node *node)
{
	/* We never need a total item count for folders.
	   Only the total unread count is interesting */
	node->itemCount = 0;
	node->unreadCount = 0;
	node_foreach_child_data (node, folder_add_child_update_counters, &node->unreadCount);
}

static void
folder_remove (Node *node)
{
	/* Ensure that there are no children anymore */
	g_assert (!node->children);
}

static void
on_newfolder_response (GtkWidget *dialog, gpointer user_data)
{
	feedlist_set_selected (feedlist_add_folder (liferea_dialog_entryrow_get (dialog, "foldertitleentry")));
}

/* folder properties dialog */

static const gchar * folder_interval_unit_options[] = {
	N_("minutes"),
	N_("hours"),
	N_("days"),
	NULL
};

gchar *
folder_describe_interval (gint interval)
{
	if (-2 == interval || 0 == interval)
		return g_strdup (_("no automatic updates"));
	if (-1 == interval) {
		gint globalInterval = 0;
		conf_get_int_value (DEFAULT_UPDATE_INTERVAL, &globalInterval);
		if (globalInterval <= 0)
			return g_strdup (_("the global default (no automatic updates)"));
		g_autofree gchar *global = folder_describe_interval (globalInterval);
		return g_strdup_printf (_("the global default (%s)"), global);
	}
	if (interval % 1440 == 0)
		return g_strdup_printf (ngettext ("every %d day", "every %d days", interval / 1440), interval / 1440);
	if (interval % 60 == 0)
		return g_strdup_printf (ngettext ("every %d hour", "every %d hours", interval / 60), interval / 60);
	return g_strdup_printf (ngettext ("every %d minute", "every %d minutes", interval), interval);
}

static gint
folder_properties_get_interval (GtkWidget *dialog)
{
	gint interval, unit;

	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderIntervalNever"))))
		return -2;
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderIntervalDefault"))))
		return -1;

	interval = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "folderIntervalSpinButton")));
	unit = gtk_drop_down_get_selected (GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "folderIntervalUnit")));
	if (1 == unit)
		interval *= 60;		/* hours */
	else if (2 == unit)
		interval *= 1440;	/* days */

	return interval;
}

/* Explains in plain words what the current choice does to the feeds inside */
static void
folder_properties_update_info (GtkWidget *dialog)
{
	Node			*node = node_from_id (g_object_get_data (G_OBJECT (dialog), "node"));
	Node			*decidingFolder = NULL;
	gboolean		override, specific;
	gint			interval;
	g_autofree gchar	*schedule = NULL, *text = NULL;

	if (!node)
		return;

	specific = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderIntervalSpecific")));
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "folderIntervalSpinButton"), specific);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "folderIntervalUnit"), specific);

	override = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderOverrideCheck")));
	interval = folder_properties_get_interval (dialog);

	/* what the folder ends up with, given its own parents */
	interval = folder_get_scheduled_interval (node, interval, &decidingFolder);
	schedule = folder_describe_interval (interval);

	if (decidingFolder && folder_get_override_children (decidingFolder))
		text = g_strdup_printf (_("The parent folder \"%s\" overrides this setting: all feeds inside use %s."), node_get_title (decidingFolder), schedule);
	else if (override)
		text = g_strdup_printf (_("All feeds inside use %s, whatever their own setting."), schedule);
	else
		text = g_strdup_printf (_("Feeds inside that use the default setting update %s. Feeds with their own setting keep it."), schedule);

	gtk_label_set_text (GTK_LABEL (liferea_dialog_lookup (dialog, "folderScheduleInfo")), text);
}

static void
on_folder_properties_changed (GtkWidget *widget, gpointer user_data)
{
	folder_properties_update_info (GTK_WIDGET (user_data));
}

static void
on_folder_properties_apply (GtkButton *button, gpointer user_data)
{
	GtkWidget	*dialog = GTK_WIDGET (user_data);
	Node		*node = node_from_id (g_object_get_data (G_OBJECT (dialog), "node"));

	if (node) {
		node_set_title (node, liferea_dialog_entryrow_get (dialog, "nameEntry"));
		folder_set_update_interval (node, folder_properties_get_interval (dialog));
		folder_set_override_children (node, gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderOverrideCheck"))));
		feed_list_view_refresh_node (node);
		feedlist_schedule_save ();
	}

	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
folder_properties_dialog (Node *node)
{
	GtkWidget		*dialog = liferea_dialog_new ("folder_properties");
	GtkDropDown		*unitDropDown;
	g_autoptr(GtkStringList) units = gtk_string_list_new (NULL);
	gint			interval, value, unit = 0;
	const gchar		*radio;

	if (!dialog)
		return;

	g_object_set_data_full (G_OBJECT (dialog), "node", g_strdup (node->id), g_free);
	liferea_dialog_entryrow_set (dialog, "nameEntry", node_get_title (node));

	for (const gchar **option = folder_interval_unit_options; *option; option++)
		gtk_string_list_append (units, _(*option));
	unitDropDown = GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "folderIntervalUnit"));
	gtk_drop_down_set_model (unitDropDown, G_LIST_MODEL (units));

	interval = folder_get_update_interval (node);
	if (-2 == interval) {
		radio = "folderIntervalNever";
	} else if (-1 == interval) {
		radio = "folderIntervalDefault";
	} else {
		radio = "folderIntervalSpecific";
	}

	/* preset the spin button with the folder's or the global interval */
	value = interval;
	if (value <= 0)
		conf_get_int_value (DEFAULT_UPDATE_INTERVAL, &value);
	if (value <= 0)
		value = 60;
	if (value % 1440 == 0) {
		unit = 2;
		value /= 1440;
	} else if (value % 60 == 0) {
		unit = 1;
		value /= 60;
	}
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "folderIntervalSpinButton")), value);
	gtk_drop_down_set_selected (unitDropDown, unit);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, radio)), TRUE);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "folderOverrideCheck")), folder_get_override_children (node));

	g_signal_connect (liferea_dialog_lookup (dialog, "folderIntervalDefault"), "toggled", G_CALLBACK (on_folder_properties_changed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "folderIntervalSpecific"), "toggled", G_CALLBACK (on_folder_properties_changed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "folderIntervalNever"), "toggled", G_CALLBACK (on_folder_properties_changed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "folderOverrideCheck"), "toggled", G_CALLBACK (on_folder_properties_changed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "folderIntervalSpinButton"), "value-changed", G_CALLBACK (on_folder_properties_changed), dialog);
	g_signal_connect_swapped (unitDropDown, "notify::selected", G_CALLBACK (folder_properties_update_info), dialog);

	g_signal_connect (liferea_dialog_lookup (dialog, "applyBtn"), "clicked", G_CALLBACK (on_folder_properties_apply), dialog);
	g_signal_connect_swapped (liferea_dialog_lookup (dialog, "cancelBtn"), "clicked", G_CALLBACK (adw_dialog_close), dialog);

	folder_properties_update_info (dialog);
}

static gboolean
folder_add_dialog (void)
{
	(void) liferea_dialog_run ("new_folder", on_newfolder_response, NULL, NULL);
	return TRUE;
}

nodeProviderPtr
folder_get_provider (void)
{
	static struct nodeProvider fnti = {
		NODE_CAPABILITY_SHOW_ITEM_FAVICONS |
		NODE_CAPABILITY_ADD_CHILDS |
		NODE_CAPABILITY_REMOVE_CHILDS |
		NODE_CAPABILITY_SUBFOLDERS |
		NODE_CAPABILITY_REORDER |
		NODE_CAPABILITY_SHOW_UNREAD_COUNT |
		NODE_CAPABILITY_UPDATE_CHILDS |
		NODE_CAPABILITY_EXPORT,
		"folder",
		ICON_FOLDER,
		folder_import,
		folder_export,
		folder_load,
		folder_update_counters,
		folder_remove,
		folder_add_dialog,
		folder_properties_dialog,
		folder_free
	};

	return &fnti;
}

nodeProviderPtr
root_get_provider (void)
{
	/* the root node is identical to the folder type,
	   just a different node type... */
	static struct nodeProvider rnti = {
		NODE_CAPABILITY_ADD_CHILDS |
		NODE_CAPABILITY_REMOVE_CHILDS |
		NODE_CAPABILITY_SUBFOLDERS |
		NODE_CAPABILITY_REORDER |
		NODE_CAPABILITY_SHOW_UNREAD_COUNT |
		NODE_CAPABILITY_UPDATE_CHILDS |
		NODE_CAPABILITY_EXPORT,
		"root",
		0,		/* and no need for an icon */
		folder_import,
		folder_export,
		folder_load,
		folder_update_counters,
		folder_remove,
		folder_add_dialog,
		feed_list_view_rename_node,
		NULL
	};

	return &rnti;
}
