/**
 * @file subscription_dialog.c  property dialog for feed subscriptions
 *
 * Copyright (C) 2004-2026 Lars Windolf <lars.windolf@gmx.de>
 * Copyright (C) 2004-2006 Nathan J. Conrad <t98502@users.sourceforge.net>
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

#include "ui/subscription_dialog.h"

#include <libxml/uri.h>
#include <libadwaita-1/adwaita.h>
#include <string.h>

#include "common.h"
#include "conf.h"
#include "db.h"
#include "debug.h"
#include "node_providers/feed.h"
#include "node_providers/folder.h"
#include "feed_parser.h"
#include "feedlist.h"
#include "html.h"
#include "node.h"
#include "update.h"
#include "xml.h"
#include "ui/feed_list_view.h"
#include "ui/liferea_dialog.h"
#include "ui/liferea_shell.h"
#include "ui/page_feed_dialog.h"
#include "ui/ui_common.h"

/* The subscription dialog can currently be used in three modes

     1. Simple (source entry only) subscribe mode -> creates a feed subscription
     2. Advanced subscribe mode -> creates a new feed subscription using all options
     3. Properties mode for existing feed subscription -> provides all options
     3. Properties mode for special subscriptions -> hides many options
*/

/* Note: these update interval literals should be kept in sync with the
   ones in ui_prefs.c! */

static const gchar * default_update_interval_unit_options[] = {
	N_("minutes"),
	N_("hours"),
	N_("days"),
	NULL
};

/* Common dialog data fields */
typedef struct ui_data {
	gint selector; /* Desiginates which fileselection dialog box is open.
				   Set to 'u' for source
				   Set to 'f' for filter */
} dialogData;

/* properties dialog */

static gchar *
ui_subscription_create_url (const gchar *url,
                            gboolean auth,
			    const gchar *username,
			    const gchar *password)
{
	gchar	*source = NULL;
	gchar *str, *tmp2;

	/* First, strip leading and trailing whitespace */
	str = g_strstrip (g_strdup (url));

	/* Add https:// if needed */
	if (strstr(str, "://") == NULL) {
		tmp2 = g_strdup_printf("https://%s",str);
		g_free(str);
		str = tmp2;
	}

	/* Add trailing / if needed */
	if (strstr(strstr(str, "://") + 3, "/") == NULL) {
		tmp2 = g_strdup_printf("%s/", str);
		g_free(str);
		str = tmp2;
	}

	/* Use the values in the textboxes if also specified in the URL! */
	if (auth) {
		xmlURIPtr uri = xmlParseURI(str);
		if (uri != NULL) {
			xmlChar *sourceUrl;
			xmlFree(uri->user);
			uri->user = g_strdup_printf("%s:%s", username, password);
			sourceUrl = xmlSaveUri(uri);
			source = g_strdup((gchar *) sourceUrl);
			g_free(uri->user);
			uri->user = NULL;
			xmlFree(sourceUrl);
			xmlFreeURI(uri);
		} else
			source = g_strdup(str);
	} else {
		source = g_strdup(str);
	}
	g_free(str);

	return source;
}

static gchar *
ui_subscription_dialog_decode_source (GtkWidget *dialog)
{
	const	gchar	*entry = liferea_dialog_entry_get (dialog, "sourceEntry");
	gchar		*source = NULL;

	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_file"))))
		source = g_strdup (entry);

	else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_url"))))
		source = ui_subscription_create_url (entry,
			gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "HTTPauthCheck"))),
			liferea_dialog_entry_get (dialog, "usernameEntry"),
			liferea_dialog_entry_get (dialog, "passwordEntry"));

	else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_cmd"))))
		source = g_strdup_printf ("|%s", entry);

	return source;
}

static void
on_dialog_response (GtkButton *btn, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	subscriptionPtr		subscription = (subscriptionPtr)g_object_get_data (G_OBJECT (dialog), "subscription");
	g_autofree gchar	*newSource = NULL;
	const gchar		*newFilter;
	gboolean		needsUpdate = FALSE;
	Node			*node = subscription->node;

	/* "General" */
	if (node) {
		node_set_title (node, liferea_dialog_entry_get (dialog, "feedNameEntry"));
	} else {
		g_autofree gchar *name = g_strstrip (g_strdup (liferea_dialog_entry_get (dialog, "feedNameEntry")));

		g_free (subscription->initialTitle);
		subscription->initialTitle = (*name)?g_steal_pointer (&name):NULL;
	}

	/* "Source" (if URL has changed...) */
	newSource = ui_subscription_dialog_decode_source (dialog);
	if (newSource && !g_str_equal (newSource, subscription_get_source (subscription))) {
		subscription_set_source (subscription, newSource);
		subscription_set_discontinued (subscription, FALSE);
		needsUpdate = TRUE;
	}

	/* Update interval handling */
	if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalNever"))))
		subscription_set_update_interval (subscription, -2);
	else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalDefault"))))
		subscription_set_update_interval (subscription, -1);
	else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalSpecific")))) {
		gint intervalUnit = gtk_combo_box_get_active (GTK_COMBO_BOX (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox")));
		gint updateInterval = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "refreshIntervalSpinButton")));
		if (intervalUnit == 1)
			updateInterval *= 60;	/* hours */
		if (intervalUnit == 2)
			updateInterval *= 1440;	/* days */

		subscription_set_update_interval (subscription, updateInterval);
	}


	if (SUBSCRIPTION_TYPE (subscription) == feed_get_subscription_type ()) {
		/* Filter handling */
		newFilter = liferea_dialog_entry_get (dialog, "filterEntry");
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "filterCheckbox"))) &&
			strcmp(newFilter,"")) { /* Maybe this should be a test to see if the file exists? */
			if (subscription_get_filter (subscription) == NULL ||
				strcmp (newFilter, subscription_get_filter (subscription))) {
				subscription_set_filter (subscription, newFilter);
				needsUpdate = TRUE;
			}
		} else {
			if (subscription_get_filter (subscription)) {
				subscription_set_filter (subscription, NULL);
				needsUpdate = TRUE;
			}
		}

		/* "Archive" handling */
		if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheDefault"))))
			subscription->cacheLimit = CACHE_DEFAULT;
		else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheDisable"))))
			subscription->cacheLimit = CACHE_DISABLE;
		else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheUnlimited"))))
			subscription->cacheLimit = CACHE_UNLIMITED;
		else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheLimited"))))
			subscription->cacheLimit = gtk_spin_button_get_value (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "cacheItemLimit")));

		if (SUBSCRIPTION_TYPE (subscription) == feed_get_subscription_type ()) {
			/* "Download" Options */
			subscription->updateOptions->dontUseProxy = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (GTK_WIDGET(dialog), "dontUseProxyCheck")));
		}

		/* "Advanced" options */
		subscription->encAutoDownload = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "enclosureDownloadCheck")));
		subscription->loadItemLink    = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "loadItemLinkCheck")));
		subscription->ignoreComments  = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "ignoreCommentFeeds")));
		subscription->markAsRead      = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "markAsReadCheck")));
		subscription->html5Extract    = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "html5ExtractCheck")));
		subscription->kdeConnect      = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "kdeConnectCheck")));
		{
			trayAlertColor oldAlert = subscription->trayAlert;

			subscription->trayAlert = gtk_drop_down_get_selected (GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "trayAlertDropDown")));
			if (subscription->trayAlert == TRAY_ALERT_NONE)
				subscription->trayAlertPending = FALSE;
			/* a colour chosen for a feed with unread items shows at once,
			   not only when the next new items arrive */
			else if (subscription->trayAlert != oldAlert && node && node->unreadCount > 0)
				subscription->trayAlertPending = TRUE;
		}
	}

	if (node) {
		/* Update existing subscription */
		feedlist_node_was_updated (node);
		db_subscription_update (subscription);
		if (needsUpdate)
			subscription_update (subscription, UPDATE_REQUEST_PRIORITY_HIGH);
	} else {
		/* Handle "Advanced" subscription */
		feedlist_add_subscription (subscription);
	}

	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_feed_prop_filtercheck (GtkCheckButton *button, gpointer user_data)
{
	gtk_widget_set_sensitive (liferea_dialog_lookup (GTK_WIDGET (user_data), "filterbox"), gtk_check_button_get_active (button));
}

static void
ui_subscription_prop_enable_httpauth (GtkWidget *dialog, gboolean enable)
{
	// FIXME: very weird logic
	gboolean on = enable && gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "HTTPauthCheck")));
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "HTTPauthCheck"), enable);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "httpAuthBox"), on);
}

static void
on_feed_prop_authcheck (GtkCheckButton *button, gpointer user_data)
{
	GtkWidget *dialog = GTK_WIDGET (user_data);

	gboolean isUrl = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_url")));

	ui_subscription_prop_enable_httpauth (dialog, isUrl);
}

static void
on_feed_prop_url_radio (GtkCheckButton *button, gpointer user_data)
{
	GtkWidget *dialog = GTK_WIDGET (user_data);
	gboolean url  = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_url")));
	gboolean file = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_file")));
	gboolean cmd  = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_cmd")));

	ui_subscription_prop_enable_httpauth (dialog, url);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "selectSourceFileButton"), file);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "filterSelectFile"), cmd);
}

static void
on_select_local_file_cb (const gchar *filename, gpointer user_data)
{
	g_autofree gchar *utfname = NULL;

	if (!filename)
		return;

	utfname = g_filename_to_utf8 (filename, -1, NULL, NULL, NULL);
	if (utfname)
		liferea_dialog_entry_set (GTK_WIDGET (user_data), "sourceEntry", utfname);
}

static void
on_select_filter_cb (const gchar *filename, gpointer user_data)
{
	g_autofree gchar *utfname = NULL;

	if (!filename)
		return;

	utfname = g_filename_to_utf8 (filename, -1, NULL, NULL, NULL);
	if (utfname)
		liferea_dialog_entry_set (GTK_WIDGET (user_data), "filterEntry", utfname);
}

static void
on_select_local_file_pressed (GtkButton *button, gpointer user_data)
{
	g_autofree gchar *name = g_filename_from_utf8 (liferea_dialog_entry_get (GTK_WIDGET (user_data), "sourceEntry"), -1, NULL, NULL, NULL);
	ui_choose_file (_("Choose File"), _("_Open"), FALSE, on_select_local_file_cb, name, NULL, NULL, NULL, user_data);
}

static void
on_select_filter_pressed (GtkButton *button, gpointer user_data)
{
	g_autofree gchar *name = g_filename_from_utf8 (liferea_dialog_entry_get (GTK_WIDGET (user_data), "filterEntry"), -1, NULL, NULL, NULL);
	ui_choose_file (_("Choose File"), _("_Open"), FALSE, on_select_filter_cb, name, NULL, NULL, NULL, user_data);
}

static void
on_feed_prop_cache_radio (GtkCheckButton *button, gpointer user_data)
{
	gboolean limited = gtk_check_button_get_active (button);

	gtk_widget_set_sensitive (liferea_dialog_lookup (GTK_WIDGET (user_data), "cacheItemLimit"), limited);
}

static void
on_feed_prop_update_radio (GtkCheckButton *button, gpointer user_data)
{
	gboolean limited = gtk_check_button_get_active (button);

	gtk_widget_set_sensitive (liferea_dialog_lookup (GTK_WIDGET (user_data), "refreshIntervalSpinButton"), limited);
	gtk_widget_set_sensitive (liferea_dialog_lookup (GTK_WIDGET (user_data), "refreshIntervalUnitComboBox"), limited);
}

/* The colour's tray icon: rounded square with the feed symbol, as the
   tray icon shows it ("None": an empty outline) */
static GdkPaintable *
tray_alert_icon_new (trayAlertColor color, gint size)
{
	static const gdouble	rgb[TRAY_ALERT_COUNT][3] = TRAY_ALERT_RGB;
	cairo_surface_t		*surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
	cairo_t			*cr = cairo_create (surface);
	const gdouble		s = size, radius = s * 0.2;
	g_autoptr(GBytes)	bytes = NULL;
	GdkTexture		*texture;

	cairo_new_sub_path (cr);
	cairo_arc (cr, s - radius - 1, radius + 1, radius, -G_PI / 2, 0);
	cairo_arc (cr, s - radius - 1, s - radius - 1, radius, 0, G_PI / 2);
	cairo_arc (cr, radius + 1, s - radius - 1, radius, G_PI / 2, G_PI);
	cairo_arc (cr, radius + 1, radius + 1, radius, G_PI, 3 * G_PI / 2);
	cairo_close_path (cr);
	if (color != TRAY_ALERT_NONE) {
		const gdouble	*c = rgb[color];
		gdouble		symbol = (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2] > 0.65)?0.2:1.0;

		cairo_set_source_rgb (cr, c[0], c[1], c[2]);
		cairo_fill_preserve (cr);
		cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
		cairo_set_line_width (cr, 1);
		cairo_stroke (cr);

		cairo_set_source_rgb (cr, symbol, symbol, symbol);
		cairo_arc (cr, s * 0.31, s * 0.69, s * 0.08, 0, 2 * G_PI);
		cairo_fill (cr);
		cairo_set_line_width (cr, s * 0.1);
		cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
		cairo_arc (cr, s * 0.25, s * 0.75, s * 0.3, -G_PI / 2, 0);
		cairo_stroke (cr);
		cairo_arc (cr, s * 0.25, s * 0.75, s * 0.52, -G_PI / 2, 0);
		cairo_stroke (cr);
	} else {
		cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.8);
		cairo_set_line_width (cr, 1.5);
		cairo_set_dash (cr, (const gdouble[]){ 2.0, 2.0 }, 2, 0);
		cairo_stroke (cr);
	}

	cairo_surface_flush (surface);
	bytes = g_bytes_new (cairo_image_surface_get_data (surface),
	                     (gsize)cairo_image_surface_get_stride (surface) * size);
	texture = gdk_memory_texture_new (size, size, GDK_MEMORY_DEFAULT, bytes,
	                                  cairo_image_surface_get_stride (surface));
	cairo_destroy (cr);
	cairo_surface_destroy (surface);

	return GDK_PAINTABLE (texture);
}

static void
on_tray_alert_item_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

	gtk_box_append (GTK_BOX (box), gtk_image_new ());
	gtk_box_append (GTK_BOX (box), gtk_label_new (NULL));
	gtk_list_item_set_child (item, box);
}

static void
on_tray_alert_item_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
	GtkWidget		*box = gtk_list_item_get_child (item);
	GtkWidget		*image = gtk_widget_get_first_child (box);
	GtkWidget		*label = gtk_widget_get_next_sibling (image);
	guint			position = gtk_list_item_get_position (item);
	g_autoptr(GdkPaintable)	icon = tray_alert_icon_new (position < TRAY_ALERT_COUNT?(trayAlertColor)position:TRAY_ALERT_NONE, 16);

	gtk_image_set_from_paintable (GTK_IMAGE (image), icon);
	gtk_label_set_text (GTK_LABEL (label), gtk_string_object_get_string (GTK_STRING_OBJECT (gtk_list_item_get_item (item))));
}

/* Collects the titles of other feeds using the given tray alert colour */
static void
subscription_dialog_collect_alert_users (Node *node, Node *self, trayAlertColor color, GSList **names)
{
	if (node != self && node->subscription && node->subscription->trayAlert == color)
		*names = g_slist_append (*names, (gpointer)node_get_title (node));

	for (GSList *iter = node->children; iter; iter = g_slist_next (iter))
		subscription_dialog_collect_alert_users ((Node *)iter->data, self, color, names);
}

/* Colours can be shared, but say so when the choice is already in use */
static void
on_tray_alert_changed (GObject *dropdown, GParamSpec *pspec, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	GtkWidget		*label = liferea_dialog_lookup (dialog, "trayAlertInfo");
	subscriptionPtr		subscription = (subscriptionPtr)g_object_get_data (G_OBJECT (dialog), "subscription");
	trayAlertColor		color = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));
	GSList			*names = NULL;
	g_autoptr(GString)	text = g_string_new (NULL);
	guint			count, shown = 0;

	if (color != TRAY_ALERT_NONE && feedlist_get_root ())
		subscription_dialog_collect_alert_users (feedlist_get_root (), subscription?subscription->node:NULL, color, &names);

	count = g_slist_length (names);
	for (GSList *iter = names; iter && shown < 3; iter = g_slist_next (iter), shown++) {
		if (shown)
			g_string_append (text, (shown == count - 1)?_(" and "):", ");
		g_string_append_printf (text, "“%s”", (gchar *)iter->data);
	}
	if (count > shown)
		g_string_append_printf (text, ngettext (" and %u other feed", " and %u other feeds", count - shown), count - shown);

	if (count) {
		g_autofree gchar *info = g_strdup_printf (_("This colour is already used by %s. They will share it."), text->str);
		gtk_label_set_text (GTK_LABEL (label), info);
	}
	gtk_widget_set_visible (label, count > 0);
	g_slist_free (names);
}

static void
subscription_prop_dialog_load (GtkWidget *dialog,
                               subscriptionPtr subscription)
{
	gint 		interval;
	gint		default_update_interval;
	gint		defaultInterval, spinSetInterval;
	gchar 		*defaultIntervalStr;
	Node		*node = subscription->node;

	/* General */
	if (node) {
		liferea_dialog_entry_set (dialog, "feedNameEntry", node_get_title (node));
	} else {
		/* new subscription: optional name (empty = use the feed title) */
		liferea_dialog_entry_set (dialog, "feedNameEntry", subscription->initialTitle?subscription->initialTitle:"");
		gtk_notebook_set_current_page (GTK_NOTEBOOK (liferea_dialog_lookup (dialog, "subscriptionPropNotebook")), 1);	// Show "Source" tab for new subscriptions
	}

	interval = subscription_get_update_interval (subscription);
	defaultInterval = update_state_get_min_interval (subscription->updateState);
	conf_get_int_value (DEFAULT_UPDATE_INTERVAL, &default_update_interval);
	spinSetInterval = defaultInterval > 0 ? defaultInterval : default_update_interval;

	if (-2 >= interval) {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalNever")), TRUE);
	} else if (-1 == interval) {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalDefault")), TRUE);
	} else {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "updateIntervalSpecific")), TRUE);
		spinSetInterval = interval;
	}

	/* Set refresh interval spin button and combo box */
	if (spinSetInterval % 1440 == 0) {	/* days */
		gtk_combo_box_set_active (GTK_COMBO_BOX (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox")), 2);
		spinSetInterval /= 1440;
	} else if (spinSetInterval % 60 == 0) {	/* hours */
		gtk_combo_box_set_active (GTK_COMBO_BOX (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox")), 1);
		spinSetInterval /= 60;
	} else {
		gtk_combo_box_set_active (GTK_COMBO_BOX (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox")), 0);
	}
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "refreshIntervalSpinButton")), spinSetInterval);

	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "refreshIntervalSpinButton"), interval > 0);
	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox"), interval > 0);

	/* setup info label about default update interval */
	if (-1 != defaultInterval)
		defaultIntervalStr = g_strdup_printf (ngettext ("The provider of this feed suggests an update interval of %d minute.",
			"The provider of this feed suggests an update interval of %d minutes.",
			defaultInterval), defaultInterval);
	else
		defaultIntervalStr = g_strdup (_("This feed specifies no default update interval."));

	/* explain folder schedules that apply to this feed */
	if (node) {
		Node *folder = NULL;
		gint scheduled = folder_get_scheduled_interval (node, -1, &folder);
		g_autofree gchar *schedule = folder_describe_interval (scheduled);
		g_autofree gchar *folderInfo = NULL;
		gchar *tmp;

		if (folder && folder_get_override_children (folder))
			folderInfo = g_strdup_printf (_("The folder \"%s\" overrides the schedule of this feed: it updates %s."), node_get_title (folder), schedule);
		else if (folder)
			folderInfo = g_strdup_printf (_("The default setting uses the schedule of the folder \"%s\": %s."), node_get_title (folder), schedule);
		else
			folderInfo = g_strdup_printf (_("The default setting uses %s."), schedule);

		tmp = g_strdup_printf ("%s\n%s", folderInfo, defaultIntervalStr);
		g_free (defaultIntervalStr);
		defaultIntervalStr = tmp;
	}

	gtk_label_set_text (GTK_LABEL (liferea_dialog_lookup (dialog, "feedUpdateInfo")), defaultIntervalStr);
	g_free (defaultIntervalStr);

	/* Source (only for feeds) */
	if (SUBSCRIPTION_TYPE (subscription) == feed_get_subscription_type ()) {
		if (subscription_get_source (subscription)[0] == '|') {
			liferea_dialog_entry_set (dialog, "sourceEntry", &(subscription_get_source (subscription)[1]));
			gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_command")), TRUE);
			ui_subscription_prop_enable_httpauth (dialog, FALSE);
			gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "selectSourceFileButton"), TRUE);
		} else if (strstr (subscription_get_source (subscription), "://") != NULL) {
			xmlURIPtr uri = xmlParseURI (subscription_get_source (subscription));
			xmlChar *parsedUrl;
			if (uri) {
				if (uri->user) {
					gchar *user = uri->user;
					gchar *pass = strstr(user, ":");
					if (pass) {
						pass[0] = '\0';
						pass++;
						liferea_dialog_entry_set (dialog, "passwordEntry", pass);
					}
					liferea_dialog_entry_set (dialog, "usernameEntry", user);
					xmlFree (uri->user);
					uri->user = NULL;
					gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "HTTPauthCheck")), TRUE);
				}
				parsedUrl = xmlSaveUri (uri);
				liferea_dialog_entry_set (dialog, "sourceEntry", (gchar *) parsedUrl);
				xmlFree (parsedUrl);
				xmlFreeURI (uri);
			} else {
				liferea_dialog_entry_set (dialog, "sourceEntry", subscription_get_source (subscription));
			}
			ui_subscription_prop_enable_httpauth (dialog, TRUE);
			gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "selectSourceFileButton"), FALSE);
		} else {
			/* File */
			liferea_dialog_entry_set (dialog, "sourceEntry", subscription_get_source (subscription));
			gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feed_loc_file")), TRUE);
			
			ui_subscription_prop_enable_httpauth (dialog, FALSE);
			gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "selectSourceFileButton"), TRUE);
		}

		if (subscription_get_filter (subscription)) {
			gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "filterCheckbox")), TRUE);
			liferea_dialog_entry_set (dialog, "filterEntry", subscription_get_filter (subscription));
		}
	}
	/* Archive */
	if (subscription->cacheLimit == CACHE_DISABLE) {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheDisable")), TRUE);
	} else if (subscription->cacheLimit == CACHE_DEFAULT) {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheDefault")), TRUE);
	} else if (subscription->cacheLimit == CACHE_UNLIMITED) {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheUnlimited")), TRUE);
	} else {
		gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "feedCacheLimited")), TRUE);
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "cacheItemLimit")), subscription->cacheLimit);
	}

	gtk_widget_set_sensitive (liferea_dialog_lookup (dialog, "cacheItemLimit"), subscription->cacheLimit > 0);

	on_feed_prop_filtercheck (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "filterCheckbox")), dialog);

	/* Download */
	gtk_check_button_set_active(GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "dontUseProxyCheck")), subscription->updateOptions->dontUseProxy);

	/* Advanced */
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "enclosureDownloadCheck")), subscription->encAutoDownload);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "loadItemLinkCheck")), subscription->loadItemLink);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "ignoreCommentFeeds")), subscription->ignoreComments);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "markAsReadCheck")), subscription->markAsRead);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "html5ExtractCheck")), subscription->html5Extract);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "kdeConnectCheck")), subscription->kdeConnect);
	{
		const gchar *colors[] = { N_("None"), N_("Blue"), N_("Green"), N_("Red"), N_("Yellow"), N_("Purple"), N_("White"), NULL };
		g_autoptr(GtkStringList) list = gtk_string_list_new (NULL);
		GtkDropDown *dropdown = GTK_DROP_DOWN (liferea_dialog_lookup (dialog, "trayAlertDropDown"));

		g_autoptr(GtkListItemFactory) factory = gtk_signal_list_item_factory_new ();

		for (guint i = 0; colors[i]; i++)
			gtk_string_list_append (list, _(colors[i]));
		g_signal_connect (factory, "setup", G_CALLBACK (on_tray_alert_item_setup), NULL);
		g_signal_connect (factory, "bind", G_CALLBACK (on_tray_alert_item_bind), NULL);
		gtk_drop_down_set_factory (dropdown, factory);
		gtk_drop_down_set_model (dropdown, G_LIST_MODEL (list));
		gtk_drop_down_set_selected (dropdown, subscription->trayAlert);
		g_signal_connect (dropdown, "notify::selected", G_CALLBACK (on_tray_alert_changed), dialog);
		on_tray_alert_changed (G_OBJECT (dropdown), NULL, dialog);
	}

	/* Remove tabs we do not need... */
	if (SUBSCRIPTION_TYPE (subscription) != feed_get_subscription_type ()) {
		/* Remove "General", "Source" and "Download" tab */
		gtk_notebook_remove_page (GTK_NOTEBOOK (liferea_dialog_lookup (dialog, "subscriptionPropNotebook")), 0);
		gtk_notebook_remove_page (GTK_NOTEBOOK (liferea_dialog_lookup (dialog, "subscriptionPropNotebook")), 0);
		gtk_notebook_remove_page (GTK_NOTEBOOK (liferea_dialog_lookup (dialog, "subscriptionPropNotebook")), 1);
	}
}

void
subscription_prop_dialog_new (subscriptionPtr subscription)
{
	GtkWidget *dialog = liferea_dialog_new ("properties");

	/* set default update interval spin button and unit combo box */
	ui_common_setup_combo_menu (liferea_dialog_lookup (dialog, "refreshIntervalUnitComboBox"),
	                            default_update_interval_unit_options,
	                            NULL /* no callback */,
	                            -1 /* default value */ );

	g_signal_connect (liferea_dialog_lookup (dialog, "selectSourceFileButton"), "clicked", G_CALLBACK (on_select_local_file_pressed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "feed_loc_url"), "toggled", G_CALLBACK (on_feed_prop_url_radio), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "feed_loc_file"), "toggled", G_CALLBACK (on_feed_prop_url_radio), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "feed_loc_command"), "toggled", G_CALLBACK (on_feed_prop_url_radio), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "HTTPauthCheck"), "toggled", G_CALLBACK (on_feed_prop_authcheck), dialog);

	g_signal_connect (liferea_dialog_lookup (dialog, "filterCheckbox"), "toggled", G_CALLBACK (on_feed_prop_filtercheck), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "filterSelectFile"), "clicked", G_CALLBACK (on_select_filter_pressed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "feedCacheLimited"), "toggled", G_CALLBACK (on_feed_prop_cache_radio), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "updateIntervalSpecific"), "toggled", G_CALLBACK(on_feed_prop_update_radio), dialog);

	g_object_set_data (G_OBJECT (dialog), "subscription", (gpointer)subscription);

	subscription_prop_dialog_load (dialog, subscription);

	g_signal_connect (liferea_dialog_lookup (dialog, "applyBtn"), "clicked", G_CALLBACK (on_dialog_response), dialog);
}

/* simple "New" dialog */

static gboolean
subscription_dialog_complex_new (gpointer user_data)
{
	subscription_prop_dialog_new ((subscriptionPtr)user_data);
	return FALSE;
}

/* Name lookup: while the user types the URL we fetch it once to fill
   the "Name" row with the feed title, which the user can then change
   before adding. Website URLs are followed to their first feed link. */

#define NAME_LOOKUP_DELAY_MS	700

static void subscription_dialog_lookup_name (GtkWidget *dialog, const gchar *url, gboolean followLinks);

static void
subscription_dialog_set_name_status (GtkWidget *dialog, const gchar *status)
{
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (liferea_dialog_lookup (dialog, "nameEntry")),
	                               status?status:_("Name"));
}

static void
on_name_changed (GtkEditable *editable, gpointer user_data)
{
	/* once the user typed a name we never replace it */
	if (!g_object_get_data (G_OBJECT (user_data), "settingName"))
		g_object_set_data (G_OBJECT (user_data), "nameEdited", GINT_TO_POINTER (TRUE));
}

/* The <title> of an HTML page (or NULL) */
static gchar *
subscription_dialog_get_page_title (const gchar *html)
{
	g_autoptr(GRegex)	regex = g_regex_new ("<title[^>]*>([^<]+)</title", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GMatchInfo)	match = NULL;

	if (!g_regex_match (regex, html, 0, &match))
		return NULL;

	return g_strstrip (unhtmlize (g_match_info_fetch (match, 1)));
}

static gboolean
subscription_dialog_name_lookup_cb (UpdateJob *job)
{
	GtkWidget		*dialog = GTK_WIDGET (job->user_data);
	UpdateResult		*result = job->result;
	gchar			*title = NULL;
	gboolean		followLinks = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "followLinks"));
	gboolean		fromPage = FALSE;

	if (!result->data || result->httpstatus < 200 || result->httpstatus >= 300) {
		subscription_dialog_set_name_status (dialog, NULL);
		return TRUE;
	}

	(void)feed_parser_probe (job->request->source, result->data, result->size, &title);

	if (!title && followLinks) {
		GSList *links = html_auto_discover_feed (result->data, job->request->source);

		/* remember the page title in case the feed has none (or there is no feed) */
		g_object_set_data_full (G_OBJECT (dialog), "pageTitle", subscription_dialog_get_page_title (result->data), g_free);
		if (links) {
			subscription_dialog_lookup_name (dialog, (const gchar *)links->data, FALSE);
			g_slist_free_full (links, g_free);
			return TRUE;
		}
	}

	if (!title && g_object_get_data (G_OBJECT (dialog), "pageTitle")) {
		title = g_strdup (g_object_get_data (G_OBJECT (dialog), "pageTitle"));
		fromPage = TRUE;
	}
	if (title)
		g_strstrip (title);

	if (title && *title && !g_object_get_data (G_OBJECT (dialog), "nameEdited")) {
		g_object_set_data (G_OBJECT (dialog), "settingName", GINT_TO_POINTER (TRUE));
		liferea_dialog_entryrow_set (dialog, "nameEntry", title);
		g_object_set_data (G_OBJECT (dialog), "settingName", NULL);
		/* a page title is only a stand-in, the feed title found later wins */
		g_object_set_data (G_OBJECT (dialog), "nameFromPage", GINT_TO_POINTER (fromPage));
	}
	subscription_dialog_set_name_status (dialog, NULL);
	g_free (title);

	return TRUE;
}

static void
subscription_dialog_lookup_name (GtkWidget *dialog, const gchar *url, gboolean followLinks)
{
	UpdateRequest *request;

	/* only the latest lookup counts */
	update_job_cancel_by_owner (dialog);

	g_object_set_data (G_OBJECT (dialog), "followLinks", GINT_TO_POINTER (followLinks));
	if (followLinks)
		g_object_set_data (G_OBJECT (dialog), "pageTitle", NULL);
	subscription_dialog_set_name_status (dialog, _("Name (looking it up...)"));
	request = update_request_new ("GET", url, NULL, NULL);
	update_job_new (dialog, request, subscription_dialog_name_lookup_cb, dialog,
	                UPDATE_REQUEST_PRIORITY_HIGH | UPDATE_REQUEST_NO_FEED);
}

static gboolean
subscription_dialog_name_lookup_timeout (gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	g_autofree gchar	*url = NULL;

	g_object_set_data (G_OBJECT (dialog), "lookupTimer", NULL);

	url = ui_subscription_create_url (liferea_dialog_entryrow_get (dialog, "sourceEntry"),
	                                  FALSE /* auth */, NULL /* user */, NULL /* passwd */);

	/* only look up web addresses with a host name */
	if (url && (g_str_has_prefix (url, "http://") || g_str_has_prefix (url, "https://")) &&
	    strchr (strstr (url, "://") + 3, '.'))
		subscription_dialog_lookup_name (dialog, url, TRUE);

	return G_SOURCE_REMOVE;
}

static void
on_source_changed (GtkEditable *editable, gpointer user_data)
{
	GtkWidget	*dialog = GTK_WIDGET (user_data);
	guint		timer = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog), "lookupTimer"));

	if (timer)
		g_source_remove (timer);
	timer = g_timeout_add (NAME_LOOKUP_DELAY_MS, subscription_dialog_name_lookup_timeout, dialog);
	g_object_set_data (G_OBJECT (dialog), "lookupTimer", GUINT_TO_POINTER (timer));
}

/* Stops pending name lookups, their callbacks must not run once the
   dialog is closed (AdwDialog emits "closed", not "destroy") */
static void
on_simple_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
	guint timer = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog), "lookupTimer"));

	if (timer)
		g_source_remove (timer);
	g_object_set_data (G_OBJECT (dialog), "lookupTimer", NULL);
	update_job_cancel_by_owner (dialog);
}

/* The name the user wants for a new subscription (or NULL): a typed
   name or one taken from the feed, but not the page title stand-in */
static gchar *
subscription_dialog_get_chosen_name (GtkWidget *dialog)
{
	g_autofree gchar *name = g_strstrip (g_strdup (liferea_dialog_entryrow_get (dialog, "nameEntry")));

	if (*name && (g_object_get_data (G_OBJECT (dialog), "nameEdited") ||
	              !g_object_get_data (G_OBJECT (dialog), "nameFromPage")))
		return g_steal_pointer (&name);

	return NULL;
}

static void
on_applybtn_clicked (GtkButton *btn, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	g_autofree gchar	*source = NULL;
	subscriptionPtr		subscription;

	source = ui_subscription_create_url (liferea_dialog_entryrow_get (dialog, "sourceEntry"),
					FALSE /* auth */, NULL /* user */, NULL /* passwd */);

	subscription = subscription_new (source, NULL, NULL);
	subscription->initialTitle = subscription_dialog_get_chosen_name (dialog);

	feedlist_add_subscription_check_duplicate (subscription);
	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_advancedbtn_clicked (GtkButton *btn, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	const gchar		*typed = liferea_dialog_entryrow_get (dialog, "sourceEntry");
	g_autofree gchar	*source = NULL;
	subscriptionPtr		subscription;

	/* carry over what was typed so far */
	if (typed && *typed)
		source = ui_subscription_create_url (typed, FALSE /* auth */, NULL /* user */, NULL /* passwd */);
	subscription = subscription_new (source?source:"https://", NULL, NULL);
	subscription->initialTitle = subscription_dialog_get_chosen_name (dialog);

	g_idle_add (subscription_dialog_complex_new, subscription);
	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_pagefeedbtn_clicked (GtkButton *btn, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	g_autofree gchar	*url = g_strdup (liferea_dialog_entryrow_get (dialog, "sourceEntry"));

	adw_dialog_close (ADW_DIALOG (dialog));
	page_feed_dialog_new (g_strstrip (url));
}

void
subscription_dialog_new (void)
{
	GtkWidget *dialog = liferea_dialog_new ("simple_subscription");
	GtkWidget *applyBtn = liferea_dialog_lookup (dialog, "applyBtn");
	AdwEntryRow *sourceEntry = ADW_ENTRY_ROW (liferea_dialog_lookup (dialog, "sourceEntry"));

	adw_dialog_set_default_widget (ADW_DIALOG (dialog), applyBtn);
	adw_entry_row_set_activates_default (sourceEntry, TRUE);
	adw_entry_row_set_activates_default (ADW_ENTRY_ROW (liferea_dialog_lookup (dialog, "nameEntry")), TRUE);

	g_signal_connect (sourceEntry, "changed", G_CALLBACK (on_source_changed), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "nameEntry"), "changed", G_CALLBACK (on_name_changed), dialog);
	g_signal_connect (dialog, "closed", G_CALLBACK (on_simple_dialog_closed), NULL);

	g_signal_connect (applyBtn, "clicked",
			  G_CALLBACK (on_applybtn_clicked),
			  dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "pageFeedBtn"), "clicked",
			  G_CALLBACK (on_pagefeedbtn_clicked),
			  dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "advancedBtn"), "clicked",
			  G_CALLBACK (on_advancedbtn_clicked),
			  dialog);
	g_signal_connect_swapped (liferea_dialog_lookup (dialog, "cancelBtn"), "clicked",
			  G_CALLBACK (adw_dialog_close),
			  dialog);

	adw_dialog_present (ADW_DIALOG (dialog), liferea_shell_get_window ());
}
