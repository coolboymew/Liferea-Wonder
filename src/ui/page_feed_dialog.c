/**
 * @file page_feed_dialog.c  setting up feeds made from web pages
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

#include "ui/page_feed_dialog.h"

#include <libadwaita-1/adwaita.h>
#include <string.h>

#include "common.h"
#include "feedlist.h"
#include "item.h"
#include "node.h"
#include "parsers/page_scraper.h"
#include "update.h"
#include "ui/feed_list_view.h"
#include "ui/liferea_dialog.h"
#include "webkit/page_browser.h"

/* The dialog fetches the page itself (through the update queue, so with
   the host throttle and cookie handling of real updates) and keeps it
   as "page" data for detection and previews. */

#define PREVIEW_ROWS	8

/* Keeps a copy of the last page looked at, to work out selectors by
   hand when detection fails */
static void
page_feed_save_page (const gchar *html)
{
	g_autofree gchar *filename = g_build_filename (g_get_user_cache_dir (), USER_DIR_NAME, "page-feed-last.html", NULL);

	if (html)
		g_file_set_contents (filename, html, -1, NULL);
}

static void
page_feed_set_status (GtkWidget *dialog, const gchar *text)
{
	gtk_label_set_text (GTK_LABEL (liferea_dialog_lookup (dialog, "previewStatus")), text?text:"");
}

static const gchar *
page_feed_entry (GtkWidget *dialog, const gchar *name)
{
	return gtk_editable_get_text (GTK_EDITABLE (liferea_dialog_lookup (dialog, name)));
}

static gchar *
page_feed_entry_dup (GtkWidget *dialog, const gchar *name)
{
	g_autofree gchar *text = g_strstrip (g_strdup (page_feed_entry (dialog, name)));

	return (*text)?g_steal_pointer (&text):NULL;
}

/* scrolling only happens in browser loads */
static guint
page_feed_scroll_seconds (GtkWidget *dialog)
{
	if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "useBrowserCheck"))))
		return 0;

	return (guint)gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "scrollSpin")));
}

static pageScraperPtr
page_feed_get_scraper (GtkWidget *dialog)
{
	pageScraperPtr scraper = page_scraper_new ();

	scraper->item = page_feed_entry_dup (dialog, "itemEntry");
	scraper->title = page_feed_entry_dup (dialog, "titleEntry");
	scraper->link = page_feed_entry_dup (dialog, "linkEntry");
	scraper->image = page_feed_entry_dup (dialog, "imageEntry");
	scraper->text = page_feed_entry_dup (dialog, "textEntry");
	scraper->price = page_feed_entry_dup (dialog, "priceEntry");

	return scraper;
}

static void
page_feed_set_entry (GtkWidget *dialog, const gchar *name, const gchar *value)
{
	gtk_editable_set_text (GTK_EDITABLE (liferea_dialog_lookup (dialog, name)), value?value:"");
}

/* the address as typed, made a URL */
static gchar *
page_feed_get_url (GtkWidget *dialog)
{
	g_autofree gchar *url = page_feed_entry_dup (dialog, "urlEntry");

	if (!url)
		return NULL;
	if (!strstr (url, "://")) {
		gchar *tmp = g_strdup_printf ("https://%s", url);
		g_free (url);
		url = tmp;
	}

	return g_steal_pointer (&url);
}

/* Shows the items the current selectors find on the fetched page */
static void
page_feed_preview (GtkWidget *dialog)
{
	const gchar		*page = g_object_get_data (G_OBJECT (dialog), "page");
	const gchar		*pageUrl = g_object_get_data (G_OBJECT (dialog), "baseUrl");
	GtkListBox		*list = GTK_LIST_BOX (liferea_dialog_lookup (dialog, "previewList"));
	pageScraperPtr		scraper = page_feed_get_scraper (dialog);
	g_autoptr(GError)	error = NULL;
	g_autofree gchar	*pageTitle = NULL, *status = NULL;
	GList			*items;
	guint			count, shown = 0;

	gtk_list_box_remove_all (list);
	gtk_widget_set_visible (GTK_WIDGET (list), FALSE);

	if (!page) {
		page_scraper_free (scraper);
		return;
	}

	items = page_scraper_extract (scraper, page, strlen (page), pageUrl, &pageTitle, &error);
	g_object_set_data_full (G_OBJECT (dialog), "pageTitle", g_strdup (pageTitle), g_free);
	page_scraper_free (scraper);

	if (error) {
		page_feed_set_status (dialog, error->message);
		return;
	}

	count = g_list_length (items);
	if (!count) {
		page_feed_set_status (dialog, _("No items found. Adjust the item block and title selectors, or try Detect."));
		return;
	}

	status = g_strdup_printf (ngettext ("Found %u item, the first ones:", "Found %u items, the first ones:", count), count);
	page_feed_set_status (dialog, status);

	for (GList *iter = items; iter && shown < PREVIEW_ROWS; iter = g_list_next (iter), shown++) {
		itemPtr		item = (itemPtr)iter->data;
		GtkWidget	*row = adw_action_row_new ();
		g_autofree gchar *details = NULL;
		const gchar	*description = item_get_description (item);

		adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), item_get_title (item));

		/* link and extra text (description without its markup) */
		if (description) {
			g_autoptr(GRegex) tags = g_regex_new ("<[^>]*>", 0, 0, NULL);
			g_autofree gchar *plain = g_regex_replace_literal (tags, description, -1, 0, " ", 0, NULL);
			details = g_strdup_printf ("%s  %s", g_strstrip (plain), item_get_source (item)?item_get_source (item):"");
		} else {
			details = g_strdup (item_get_source (item));
		}
		adw_action_row_set_subtitle (ADW_ACTION_ROW (row), details?details:"");
		adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 2);
		gtk_list_box_append (list, row);
	}
	gtk_widget_set_visible (GTK_WIDGET (list), TRUE);

	g_list_free_full (items, g_object_unref);
}

static gboolean
page_feed_fetch_cb (UpdateJob *job)
{
	GtkWidget	*dialog = GTK_WIDGET (job->user_data);
	gboolean	detect = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "detect"));

	if (job->result->siteCheckNeeded) {
		page_feed_set_status (dialog, _("The site asks for a browser check: click \"Open Page...\", pass the check in that window, close it and preview again."));
		g_object_set_data (G_OBJECT (dialog), "page", NULL);
		return TRUE;
	}

	if (!job->result->data || job->result->httpstatus < 200 || job->result->httpstatus >= 300) {
		g_autofree gchar *text = g_strdup_printf (_("The page could not be loaded (HTTP %d)."), job->result->httpstatus);
		page_feed_set_status (dialog, text);
		g_object_set_data (G_OBJECT (dialog), "page", NULL);
		return TRUE;
	}

	g_object_set_data_full (G_OBJECT (dialog), "page", g_strndup (job->result->data, job->result->size), g_free);
	page_feed_save_page (g_object_get_data (G_OBJECT (dialog), "page"));
	g_object_set_data_full (G_OBJECT (dialog), "pageUrl", g_strdup (job->request->source), g_free);
	/* relative links resolve against where the page really came from */
	g_object_set_data_full (G_OBJECT (dialog), "baseUrl", g_strdup (job->result->source?job->result->source:job->request->source), g_free);

	if (detect) {
		pageScraperPtr scraper = page_scraper_detect (job->result->data, job->result->size);

		if (!scraper) {
			page_feed_set_status (dialog, _("No repeating item blocks found. Enter the selectors yourself (your browser's \"Inspect\" shows the page's classes)."));
			return TRUE;
		}
		page_feed_set_entry (dialog, "itemEntry", scraper->item);
		page_feed_set_entry (dialog, "titleEntry", scraper->title);
		page_feed_set_entry (dialog, "linkEntry", scraper->link);
		page_feed_set_entry (dialog, "imageEntry", scraper->image);
		page_feed_set_entry (dialog, "textEntry", scraper->text);
		page_feed_set_entry (dialog, "priceEntry", scraper->price);
		page_scraper_free (scraper);
	}

	page_feed_preview (dialog);
	return TRUE;
}

static void
page_feed_fetch (GtkWidget *dialog, gboolean detect, gboolean force)
{
	g_autofree gchar	*url = page_feed_get_url (dialog);
	const gchar		*pageUrl = g_object_get_data (G_OBJECT (dialog), "pageUrl");
	struct updateOptions	options = { 0 };
	UpdateRequest		*request;

	if (!url) {
		page_feed_set_status (dialog, _("Enter the page address first."));
		return;
	}

	/* same page already loaded: no need to ask the site again */
	if (!detect && !force && g_object_get_data (G_OBJECT (dialog), "page") && !g_strcmp0 (url, pageUrl)) {
		page_feed_preview (dialog);
		return;
	}

	update_job_cancel_by_owner (dialog);
	g_object_set_data (G_OBJECT (dialog), "detect", GINT_TO_POINTER (detect));
	page_feed_set_status (dialog, _("Loading the page..."));

	options.keepCookies = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "keepCookiesCheck")));
	options.useBrowser = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "useBrowserCheck")));
	options.waitFor = page_feed_entry_dup (dialog, "itemEntry");
	options.scrollSeconds = page_feed_scroll_seconds (dialog);
	request = update_request_new ("GET", url, NULL, &options);
	update_job_new (dialog, request, page_feed_fetch_cb, dialog, UPDATE_REQUEST_PRIORITY_HIGH | UPDATE_REQUEST_NO_FEED);
	g_free (options.waitFor);
}

static void
on_detect_clicked (GtkButton *button, gpointer user_data)
{
	page_feed_fetch (GTK_WIDGET (user_data), TRUE, FALSE);
}

static void
on_preview_clicked (GtkButton *button, gpointer user_data)
{
	page_feed_fetch (GTK_WIDGET (user_data), FALSE, FALSE);
}

/* Takes a page as shown in the browser window (fully loaded, after any
   check or section visit) at the address it is on now */
static void
page_feed_use_page (GtkWidget *dialog, const gchar *uri, const gchar *html, gboolean detect)
{
	if (uri && *uri)
		page_feed_set_entry (dialog, "urlEntry", uri);

	page_feed_save_page (html);
	g_object_set_data_full (G_OBJECT (dialog), "page", g_strdup (html), g_free);
	g_object_set_data_full (G_OBJECT (dialog), "pageUrl", page_feed_get_url (dialog), g_free);
	g_object_set_data_full (G_OBJECT (dialog), "baseUrl", page_feed_get_url (dialog), g_free);

	if (detect) {
		pageScraperPtr scraper = page_scraper_detect (html, strlen (html));

		if (!scraper) {
			page_feed_set_status (dialog, _("No repeating item blocks found on this page. Enter the selectors yourself (your browser's \"Inspect\" shows the page's classes)."));
			return;
		}
		page_feed_set_entry (dialog, "itemEntry", scraper->item);
		page_feed_set_entry (dialog, "titleEntry", scraper->title);
		page_feed_set_entry (dialog, "linkEntry", scraper->link);
		page_feed_set_entry (dialog, "imageEntry", scraper->image);
		page_feed_set_entry (dialog, "textEntry", scraper->text);
		page_feed_set_entry (dialog, "priceEntry", scraper->price);
		page_scraper_free (scraper);
	}
	page_feed_preview (dialog);
}

/* Selectors taken from the browser window fit the page as its JavaScript
   built it, so the feed must be loaded the same way */
static void
page_feed_use_browser (GtkWidget *dialog)
{
	GtkCheckButton *check = GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "useBrowserCheck"));

	if (gtk_check_button_get_active (check))
		return;

	gtk_check_button_set_active (check, TRUE);
	page_feed_set_status (dialog, _("Loading with the built-in browser is now on: the items were found on the page as the browser shows it."));
}

/* "Detect Items Here" in the browser window */
static void
on_open_page_detect (gpointer data, const gchar *uri, const gchar *html)
{
	GtkWidget *dialog = GTK_WIDGET (data);

	if (gtk_widget_get_root (dialog) && html) {
		/* closing the window afterwards keeps this result */
		g_object_set_data (G_OBJECT (dialog), "usedWindowPage", GINT_TO_POINTER (TRUE));
		page_feed_use_browser (dialog);
		page_feed_use_page (dialog, uri, html, TRUE);
	}
}

/* "Pick an Item" in the browser window: the user's choice of item */
static void
on_open_page_pick (gpointer data, const gchar *uri, const gchar *html,
                   const gchar *item, const gchar *title, const gchar *image, const gchar *text,
                   const gchar *price)
{
	GtkWidget *dialog = GTK_WIDGET (data);

	if (!gtk_widget_get_root (dialog) || !item)
		return;

	g_object_set_data (G_OBJECT (dialog), "usedWindowPage", GINT_TO_POINTER (TRUE));
	page_feed_use_browser (dialog);
	page_feed_set_entry (dialog, "itemEntry", item);
	page_feed_set_entry (dialog, "titleEntry", title);
	page_feed_set_entry (dialog, "linkEntry", NULL);
	page_feed_set_entry (dialog, "imageEntry", image);
	page_feed_set_entry (dialog, "textEntry", text);
	page_feed_set_entry (dialog, "priceEntry", price);
	page_feed_use_page (dialog, uri, html, FALSE);
}

/* Closing the window without detecting there: the feed's settings stay
   as they were (address, browser loading), only the preview is fetched
   again since the window may have passed a site check or set cookies.
   Looking at a page must not change a feed that works: the window may
   end up at another address (redirects, links clicked) and shows the
   page as rebuilt by its JavaScript, which plain downloads do not get. */
static void
on_open_page_closed (gpointer data, const gchar *uri, const gchar *html)
{
	GtkWidget		*dialog = GTK_WIDGET (data);
	g_autofree gchar	*item = NULL;

	if (!gtk_widget_get_root (dialog) || g_object_get_data (G_OBJECT (dialog), "usedWindowPage"))
		return;

	item = page_feed_entry_dup (dialog, "itemEntry");
	page_feed_fetch (dialog, !item, TRUE);
}

static void
on_open_page_clicked (GtkButton *button, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	g_autofree gchar	*url = page_feed_get_url (dialog);

	if (!url) {
		page_feed_set_status (dialog, _("Enter the page address first."));
		return;
	}

	g_object_set_data (G_OBJECT (dialog), "usedWindowPage", NULL);
	page_browser_open_window (url, _("_Detect Items Here"), on_open_page_detect, on_open_page_pick,
	                          on_open_page_closed, g_object_ref (dialog), g_object_unref);
}

static void
on_page_feed_apply (GtkButton *button, gpointer user_data)
{
	GtkWidget		*dialog = GTK_WIDGET (user_data);
	subscriptionPtr		subscription = g_object_get_data (G_OBJECT (dialog), "subscription");
	g_autofree gchar	*url = page_feed_get_url (dialog);
	g_autofree gchar	*name = page_feed_entry_dup (dialog, "nameEntry");
	pageScraperPtr		scraper = page_feed_get_scraper (dialog);
	gboolean		keepCookies = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "keepCookiesCheck")));
	gboolean		useBrowser = gtk_check_button_get_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "useBrowserCheck")));
	guint			scrollSeconds = page_feed_scroll_seconds (dialog);
	g_autoptr(GError)	error = NULL;
	g_autofree gchar	*xpath = NULL;

	/* the essentials must be there and valid */
	if (!url || !scraper->item || !scraper->title) {
		page_feed_set_status (dialog, _("The page address, the item block and the title are needed."));
		page_scraper_free (scraper);
		return;
	}
	xpath = page_scraper_css_to_xpath (scraper->item, FALSE, &error);
	if (!xpath) {
		page_feed_set_status (dialog, error->message);
		page_scraper_free (scraper);
		return;
	}

	/* an unnamed new feed is named after the page */
	if (!name && g_object_get_data (G_OBJECT (dialog), "pageTitle"))
		name = g_strdup (g_object_get_data (G_OBJECT (dialog), "pageTitle"));

	if (subscription) {
		Node *node = subscription->node;

		page_scraper_free (subscription->scraper);
		subscription->scraper = scraper;
		subscription->updateOptions->keepCookies = keepCookies;
		subscription->updateOptions->useBrowser = useBrowser;
		subscription->updateOptions->scrollSeconds = scrollSeconds;
		if (!g_str_equal (url, subscription_get_source (subscription)))
			subscription_set_source (subscription, url);
		if (name && node) {
			node_set_title (node, name);
			feed_list_view_refresh_node (node);
		}
		feedlist_schedule_save ();
		subscription_update (subscription, UPDATE_REQUEST_PRIORITY_HIGH);
	} else {
		subscription = subscription_new (url, NULL, NULL);
		subscription->scraper = scraper;
		subscription->updateOptions->keepCookies = keepCookies;
		subscription->updateOptions->useBrowser = useBrowser;
		subscription->updateOptions->scrollSeconds = scrollSeconds;
		subscription->initialTitle = g_steal_pointer (&name);
		feedlist_add_subscription_check_duplicate (subscription);
	}

	adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_page_feed_closed (AdwDialog *dialog, gpointer user_data)
{
	update_job_cancel_by_owner (dialog);
}

static GtkWidget *
page_feed_dialog_create (void)
{
	GtkWidget *dialog = liferea_dialog_new ("page_feed");

	if (!dialog)
		return NULL;

	g_signal_connect (liferea_dialog_lookup (dialog, "detectBtn"), "clicked", G_CALLBACK (on_detect_clicked), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "previewBtn"), "clicked", G_CALLBACK (on_preview_clicked), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "openPageBtn"), "clicked", G_CALLBACK (on_open_page_clicked), dialog);
	g_signal_connect (liferea_dialog_lookup (dialog, "applyBtn"), "clicked", G_CALLBACK (on_page_feed_apply), dialog);
	g_signal_connect_swapped (liferea_dialog_lookup (dialog, "cancelBtn"), "clicked", G_CALLBACK (adw_dialog_close), dialog);
	g_signal_connect (dialog, "closed", G_CALLBACK (on_page_feed_closed), NULL);

	return dialog;
}

void
page_feed_dialog_new (const gchar *url)
{
	GtkWidget *dialog = page_feed_dialog_create ();

	if (!dialog)
		return;

	if (url && *url && !g_str_equal (url, "https://")) {
		page_feed_set_entry (dialog, "urlEntry", url);
		/* most pages need the selectors detected anyway */
		page_feed_fetch (dialog, TRUE, FALSE);
	}
}

void
page_feed_dialog_edit (subscriptionPtr subscription)
{
	GtkWidget	*dialog = page_feed_dialog_create ();
	pageScraperPtr	scraper = subscription->scraper;

	if (!dialog || !scraper)
		return;

	adw_dialog_set_title (ADW_DIALOG (dialog), _("Edit Feed From Web Page"));
	gtk_button_set_label (GTK_BUTTON (liferea_dialog_lookup (dialog, "applyBtn")), _("_Save"));
	g_object_set_data (G_OBJECT (dialog), "subscription", subscription);

	page_feed_set_entry (dialog, "urlEntry", subscription_get_source (subscription));
	if (subscription->node)
		page_feed_set_entry (dialog, "nameEntry", node_get_title (subscription->node));
	page_feed_set_entry (dialog, "itemEntry", scraper->item);
	page_feed_set_entry (dialog, "titleEntry", scraper->title);
	page_feed_set_entry (dialog, "linkEntry", scraper->link);
	page_feed_set_entry (dialog, "imageEntry", scraper->image);
	page_feed_set_entry (dialog, "textEntry", scraper->text);
	page_feed_set_entry (dialog, "priceEntry", scraper->price);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "keepCookiesCheck")), subscription->updateOptions->keepCookies);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (liferea_dialog_lookup (dialog, "useBrowserCheck")), subscription->updateOptions->useBrowser);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (liferea_dialog_lookup (dialog, "scrollSpin")), subscription->updateOptions->scrollSeconds);

	/* show what the current selectors give */
	page_feed_fetch (dialog, FALSE, FALSE);
}
