/*
 * @file page_browser.h  loading page feeds with the built-in browser engine
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

#ifndef _PAGE_BROWSER_H
#define _PAGE_BROWSER_H

#include "update_job.h"

/* Page feeds with "load in browser" are loaded by a WebKit view instead
   of a plain download: the page's JavaScript runs and the finished page
   is read. All these views share one persistent browser session (own
   cookies and storage, kept across restarts, separate from Liferea's
   item view which forgets its data on every start). When a site shows a
   browser check (e.g. Cloudflare "Just a moment..."), the feed is marked
   as needing a site check: the user passes it in a visible window of the
   same session and later loads are let through like any browser visit.
   Nothing here tries to get around such checks. */

/**
 * page_browser_process_request: (skip)
 * @job:	the update job of a subscription with useBrowser set
 *
 * Loads the page in a hidden browser view and finishes the job with
 * the page's HTML, or fails it with siteCheckNeeded set.
 */
void page_browser_process_request (UpdateJob *job);

/**
 * page_browser_is_site_check: (skip)
 * @title:	the page title
 * @html:	the page HTML
 *
 * Returns: TRUE if the page is a browser check page instead of content
 */
gboolean page_browser_is_site_check (const gchar *title, const gchar *html);

/* gets the page's current address and HTML (NULL if not readable or still a check page) */
typedef void (*PageBrowserPageFunc) (gpointer user_data, const gchar *uri, const gchar *html);

/* gets the page and the selectors of an item the user picked on it */
typedef void (*PageBrowserPickFunc) (gpointer user_data, const gchar *uri, const gchar *html,
                                     const gchar *item, const gchar *title, const gchar *image, const gchar *text,
                                     const gchar *price);

/**
 * page_browser_open_window: (skip)
 * @url:	the page to open
 * @actionLabel: (nullable): label of a title bar button handing the page
 *			as shown to @action (e.g. "Detect Items Here")
 * @action: (nullable):	called when that button is clicked
 * @pick: (nullable):	adds a "Pick an Item" button, called with the
 *			selectors of the item the user clicked on the page
 * @closed: (nullable):	called when the window is closed
 * @user_data:	(transfer full): passed to the callbacks, freed with @destroy
 * @destroy: (nullable): frees @user_data
 *
 * Opens the page in a visible window of the page feed browser session,
 * e.g. to pass a site's browser check, confirm one's age, log in or
 * pick the section whose items the feed should show.
 */
void page_browser_open_window (const gchar *url, const gchar *actionLabel, PageBrowserPageFunc action,
                               PageBrowserPickFunc pick, PageBrowserPageFunc closed,
                               gpointer user_data, GDestroyNotify destroy);

#endif
