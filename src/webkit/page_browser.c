/**
 * @file page_browser.c  loading page feeds with the built-in browser engine
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

#include "webkit/page_browser.h"

#include <json-glib/json-glib.h>
#include <libadwaita-1/adwaita.h>
#include <stdlib.h>
#include <string.h>
#include <webkit/webkit.h>

#include "common.h"
#include "debug.h"
#include "ui/liferea_shell.h"

/* the page is read when its HTML stopped changing between two looks
   this far apart (its JavaScript may still be loading the list) */
#define PAGE_BROWSER_SETTLE_MS		1500
/* with a selector to wait for, up to 30s for the items to show up */
#define PAGE_BROWSER_MAX_POLLS		20
/* how often a check page is looked at again (checks without a question
   to the user usually finish by themselves within seconds) */
#define PAGE_BROWSER_CHECK_RETRIES	4
#define PAGE_BROWSER_TIMEOUT_SECONDS	60

/* size of the hidden view when scrolling (pages load more items when
   their end comes into view, a view without size has no view) */
#define PAGE_BROWSER_SCROLL_WIDTH	1280
#define PAGE_BROWSER_SCROLL_HEIGHT	1600

/* Hidden views count as not shown: the page's timers for "is this in
   view" and animation frames are paused, so lists that load more while
   scrolling would never load. Run before the page's own scripts when
   scrolling, this stands in for them with plain timers. */
static const gchar *page_browser_scroll_helper_js =
	"(function () {\n"
	"\ttry {\n"
	"\t\tObject.defineProperty (document, 'visibilityState', { get: function () { return 'visible'; } });\n"
	"\t\tObject.defineProperty (document, 'hidden', { get: function () { return false; } });\n"
	"\t} catch (e) {}\n"
	"\twindow.requestAnimationFrame = function (cb) { return setTimeout (function () { cb (performance.now ()); }, 16); };\n"
	"\twindow.cancelAnimationFrame = function (id) { clearTimeout (id); };\n"
	"\tfunction margin (m) {\n"
	"\t\tvar p = (m || '0px').split (/\\s+/).map (function (v) { return parseFloat (v) || 0; });\n"
	"\t\twhile (p.length < 4) p.push (p[p.length > 1 ? p.length - 2 : 0]);\n"
	"\t\treturn p;\n"
	"\t}\n"
	"\tfunction IO (cb, opts) {\n"
	"\t\tthis.cb = cb; this.targets = []; this.state = new Map ();\n"
	"\t\tthis.m = margin (opts && opts.rootMargin);\n"
	"\t\tvar self = this;\n"
	"\t\tthis.timer = setInterval (function () { self.check (); }, 200);\n"
	"\t}\n"
	"\tIO.prototype.check = function () {\n"
	"\t\tvar vh = window.innerHeight || 1600, vw = window.innerWidth || 1280, m = this.m, entries = [];\n"
	"\t\tfor (var i = 0; i < this.targets.length; i++) {\n"
	"\t\t\tvar t = this.targets[i], r = t.getBoundingClientRect ();\n"
	"\t\t\tvar on = r.bottom >= -m[0] && r.top <= vh + m[2] && r.right >= -m[3] && r.left <= vw + m[1];\n"
	"\t\t\tif (this.state.get (t) !== on) {\n"
	"\t\t\t\tthis.state.set (t, on);\n"
	"\t\t\t\tentries.push ({ target: t, isIntersecting: on, intersectionRatio: on ? 1 : 0,\n"
	"\t\t\t\t                boundingClientRect: r, intersectionRect: on ? r : null, rootBounds: null, time: performance.now () });\n"
	"\t\t\t}\n"
	"\t\t}\n"
	"\t\tif (entries.length) this.cb (entries, this);\n"
	"\t};\n"
	"\tIO.prototype.observe = function (t) { if (this.targets.indexOf (t) < 0) this.targets.push (t); };\n"
	"\tIO.prototype.unobserve = function (t) { this.targets = this.targets.filter (function (x) { return x !== t; }); this.state.delete (t); };\n"
	"\tIO.prototype.disconnect = function () { this.targets = []; this.state.clear (); };\n"
	"\tIO.prototype.takeRecords = function () { return []; };\n"
	"\twindow.IntersectionObserver = IO;\n"
	"}) ();\n";

/* scrolls to the end of the page every 300ms for %u ms; at the end it
   goes back up a screen first, so the end leaves the view and comes into
   it again (pages load more when it comes into view, not while it is) */
#define PAGE_BROWSER_SCROLL_JS	"(function () { var end = Date.now () + %u, t = setInterval (function () {" \
				" var e = document.scrollingElement || document.documentElement;" \
				" if (e.scrollTop + window.innerHeight >= e.scrollHeight - 2) e.scrollTop -= window.innerHeight;" \
				" else e.scrollTop = e.scrollHeight;" \
				" window.dispatchEvent (new Event ('scroll'));" \
				" if (Date.now () > end) { clearInterval (t); e.scrollTop = e.scrollHeight; } }, 300); }) ()"

/* returns the page title and HTML separated by \x01; pages built of web
   components keep parts in shadow roots, which outerHTML leaves out:
   those are written into their elements (as templates) */
#define PAGE_BROWSER_SNAPSHOT_JS	"(document.title || '') + '\\x01' + (function () {" \
	" var roots = [];" \
	" function collect (r) { var all = r.querySelectorAll ('*');" \
	"  for (var i = 0; i < all.length; i++) if (all[i].shadowRoot) { roots.push (all[i].shadowRoot); collect (all[i].shadowRoot); } }" \
	" try { collect (document);" \
	"  if (roots.length && document.documentElement.getHTML)" \
	"   return '<html>' + document.documentElement.getHTML ({ shadowRoots: roots }) + '</html>';" \
	" } catch (e) {}" \
	" return document.documentElement.outerHTML; }) ()"

/* Like PAGE_BROWSER_SNAPSHOT_JS, prefixed with the number of elements the
   selector matches and \x02 (-1 without selector, -2 if not understood) */
static gchar *
page_browser_snapshot_js (const gchar *selector)
{
	g_autoptr(JsonNode)	node = json_node_new (JSON_NODE_VALUE);
	g_autofree gchar	*literal = NULL;

	json_node_set_string (node, selector?selector:"");
	literal = json_to_string (node, FALSE);

	return g_strdup_printf ("(function () { var s = %s, n = -1;"
	                        " try { if (s) n = /^(\\/|\\.\\/|\\()/.test (s)"
	                        "   ? document.evaluate ('count(' + s + ')', document, null, XPathResult.NUMBER_TYPE, null).numberValue"
	                        "   : document.querySelectorAll (s).length; } catch (e) { n = -2; }"
	                        " return n + '\\x02' + " PAGE_BROWSER_SNAPSHOT_JS "; }) ()", literal);
}

static WebKitNetworkSession *session = NULL;

static WebKitNetworkSession *
page_browser_get_session (void)
{
	if (!session) {
		g_autofree gchar *dataDir = g_build_filename (g_get_user_data_dir (), USER_DIR_NAME, "page-browser", NULL);
		g_autofree gchar *cacheDir = g_build_filename (g_get_user_cache_dir (), USER_DIR_NAME, "page-browser", NULL);
		g_autofree gchar *cookies = g_build_filename (dataDir, "cookies.sqlite", NULL);
		WebKitCookieManager *cookieManager;

		g_mkdir_with_parents (dataDir, 0700);
		session = webkit_network_session_new (dataDir, cacheDir);
		cookieManager = webkit_network_session_get_cookie_manager (session);
		webkit_cookie_manager_set_persistent_storage (cookieManager, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
		webkit_cookie_manager_set_accept_policy (cookieManager, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
	}

	return session;
}

WebKitCookieManager *
page_browser_get_cookie_manager (void)
{
	return webkit_network_session_get_cookie_manager (page_browser_get_session ());
}

static WebKitWebView *
page_browser_view_new (void)
{
	WebKitWebView *view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
	                                                     "network-session", page_browser_get_session (),
	                                                     NULL));

	WebKitSettings *settings = webkit_web_view_get_settings (view);

	webkit_settings_set_enable_javascript (settings, TRUE);
	/* reading pages never needs camera or microphone */
	webkit_settings_set_enable_media_stream (settings, FALSE);
	webkit_settings_set_media_playback_requires_user_gesture (settings, TRUE);
	/* nor videos at all: lighter, and video sites crashed the
	   page right away where GStreamer's base plugins are missing */
	webkit_settings_set_enable_media (settings, FALSE);
	webkit_settings_set_enable_mediasource (settings, FALSE);
	webkit_settings_set_enable_encrypted_media (settings, FALSE);
	return view;
}

/* Hidden loads are never shown: no graphics card use, no 3D or sound
   (no video anywhere, see page_browser_view_new), which also keeps heavy
   pages light in the background. The visible window keeps WebGL and web
   audio, some site checks use them. */
static void
page_browser_view_make_hidden (WebKitWebView *view)
{
	WebKitSettings *settings = webkit_web_view_get_settings (view);

	webkit_settings_set_hardware_acceleration_policy (settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
	webkit_settings_set_enable_webgl (settings, FALSE);
	webkit_settings_set_enable_webaudio (settings, FALSE);
	webkit_settings_set_enable_media (settings, FALSE);
	webkit_settings_set_enable_mediasource (settings, FALSE);
	webkit_settings_set_enable_page_cache (settings, FALSE);
}

/* Check pages (Cloudflare "Just a moment...") and pages behind them are
   told apart by the title and by the check's own page setup. Cloudflare
   also puts a small "challenge-platform" script and Turnstile boxes
   into normal pages of protected sites, those do not count (they kept
   feeds marked as waiting for a check after it was passed). */
gboolean
page_browser_is_site_check (const gchar *title, const gchar *html)
{
	if (title && (g_str_has_prefix (title, "Just a moment") ||
	              g_str_has_prefix (title, "Attention Required") ||
	              g_str_has_prefix (title, "One moment, please")))
		return TRUE;

	return html && (strstr (html, "window._cf_chl_opt") ||
	                strstr (html, "/cdn-cgi/challenge-platform/h/") ||
	                strstr (html, "id=\"challenge-form\"") ||
	                strstr (html, "id=\"challenge-running\""));
}

/* Pages the user passed a check on, as shown in the window: the next
   load of the same address uses it instead of loading again (a second
   load in a hidden view may get another check) */

#define PAGE_BROWSER_OFFER_SECONDS	300

typedef struct {
	gchar	*html;
	gint64	time;
} pageOffer;

static GHashTable *offers = NULL;

static void
page_offer_free (gpointer data)
{
	pageOffer *offer = (pageOffer *)data;

	g_free (offer->html);
	g_free (offer);
}

void
page_browser_offer_page (const gchar *url, const gchar *html)
{
	pageOffer *offer;

	if (!url || !html)
		return;
	if (!offers)
		offers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, page_offer_free);

	offer = g_new0 (pageOffer, 1);
	offer->html = g_strdup (html);
	offer->time = g_get_monotonic_time ();
	g_hash_table_replace (offers, g_strdup (url), offer);
}

static gchar *
page_browser_take_offer (const gchar *url)
{
	pageOffer	*offer = offers?g_hash_table_lookup (offers, url):NULL;
	gchar		*html = NULL;

	if (offer && g_get_monotonic_time () - offer->time < PAGE_BROWSER_OFFER_SECONDS * G_USEC_PER_SEC)
		html = g_steal_pointer (&offer->html);
	if (offer)
		g_hash_table_remove (offers, url);

	return html;
}

/* hidden loading for updates */

typedef struct {
	UpdateJob	*job;
	WebKitWebView	*view;
	guint		settleId;
	guint		timeoutId;
	guint		checks;
	guint		polls;
	gsize		lastSize;
	gchar		*waitFor;
	guint		scrollSeconds;
	gboolean	done;
} pageFetch;

static void
page_fetch_free (gpointer data)
{
	pageFetch *fetch = (pageFetch *)data;

	g_clear_handle_id (&fetch->settleId, g_source_remove);
	g_clear_handle_id (&fetch->timeoutId, g_source_remove);
	g_free (fetch->waitFor);
	if (fetch->view) {
		g_signal_handlers_disconnect_by_data (fetch->view, fetch);
		webkit_web_view_stop_loading (fetch->view);
		g_object_unref (fetch->view);
	}
	g_free (fetch);
}

static gboolean
page_fetch_free_idle (gpointer data)
{
	page_fetch_free (data);
	return G_SOURCE_REMOVE;
}

/* finishes the job once, the view is dropped outside of its callbacks */
static void
page_fetch_done (pageFetch *fetch, gchar *html, const gchar *error, gboolean siteCheck)
{
	UpdateJob *job = fetch->job;

	if (fetch->done) {
		g_free (html);
		return;
	}
	fetch->done = TRUE;

	job->result->source = g_strdup (webkit_web_view_get_uri (fetch->view)?webkit_web_view_get_uri (fetch->view):job->request->source);
	if (html) {
		job->result->httpstatus = 200;
		job->result->size = strlen (html);
		job->result->data = html;
		update_job_finished (job);
	} else {
		job->result->httpstatus = siteCheck?403:0;
		job->result->siteCheckNeeded = siteCheck;
		update_job_failed (job, g_strdup (error));
	}

	g_idle_add (page_fetch_free_idle, fetch);
}

static gboolean page_fetch_snapshot (gpointer user_data);

static void
page_fetch_settle (pageFetch *fetch)
{
	g_clear_handle_id (&fetch->settleId, g_source_remove);
	fetch->settleId = g_timeout_add (PAGE_BROWSER_SETTLE_MS, page_fetch_snapshot, fetch);
}

static void
page_fetch_snapshot_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
	pageFetch		*fetch = (pageFetch *)user_data;
	g_autoptr(JSCValue)	value = NULL;
	g_autoptr(GError)	error = NULL;
	g_autofree gchar	*snapshot = NULL;
	gchar			*html, *title;
	gint			matches;

	value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), res, &error);
	if (fetch->done)
		return;
	if (!value) {
		page_fetch_done (fetch, NULL, error?error->message:_("The page could not be read."), FALSE);
		return;
	}

	snapshot = jsc_value_to_string (value);
	title = strchr (snapshot, '\x02');
	matches = title?atoi (snapshot):-1;
	title = title?title + 1:snapshot;
	html = strchr (title, '\x01');
	if (html)
		*html++ = 0;
	else
		html = title;

	if (page_browser_is_site_check (title, html)) {
		/* automatic checks finish by themselves, give them time */
		if (++fetch->checks <= PAGE_BROWSER_CHECK_RETRIES) {
			debug (DEBUG_UPDATE, "page browser: site check on %s, waiting (%u)", fetch->job->request->source, fetch->checks);
			page_fetch_settle (fetch);
			return;
		}
		page_fetch_done (fetch, NULL, _("The site asks for a browser check. Right-click the feed and choose \"Pass Site Check...\" to do it once in a browser window."), TRUE);
		return;
	}

	/* Not loaded yet: with a selector, until it matches (pages load
	   their lists late), otherwise until the HTML stopped changing */
	if (++fetch->polls < PAGE_BROWSER_MAX_POLLS) {
		gboolean ready;

		if (matches >= 0)
			ready = (matches > 0);
		else
			ready = (strlen (html) == fetch->lastSize);
		fetch->lastSize = strlen (html);

		if (!ready) {
			page_fetch_settle (fetch);
			return;
		}
	}

	debug (DEBUG_UPDATE, "page browser: read %s (%zu bytes, %u looks, %d items matched)", fetch->job->request->source, strlen (html), fetch->polls, matches);
	page_fetch_done (fetch, g_strdup (html), NULL, FALSE);
}

static gboolean
page_fetch_snapshot (gpointer user_data)
{
	pageFetch *fetch = (pageFetch *)user_data;

	fetch->settleId = 0;
	g_autofree gchar *script = page_browser_snapshot_js (fetch->waitFor);

	webkit_web_view_evaluate_javascript (fetch->view, script, -1, NULL, NULL, NULL,
	                                     page_fetch_snapshot_cb, fetch);
	return G_SOURCE_REMOVE;
}

static void
on_fetch_load_changed (WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
	pageFetch *fetch = (pageFetch *)user_data;

	/* also after redirects or reloads done by check pages */
	if (WEBKIT_LOAD_FINISHED != event)
		return;

	if (fetch->scrollSeconds) {
		g_autofree gchar *script = g_strdup_printf (PAGE_BROWSER_SCROLL_JS, fetch->scrollSeconds * 1000);

		debug (DEBUG_UPDATE, "page browser: scrolling %s for %us", fetch->job->request->source, fetch->scrollSeconds);
		webkit_web_view_evaluate_javascript (view, script, -1, NULL, NULL, NULL, NULL, NULL);
		g_clear_handle_id (&fetch->settleId, g_source_remove);
		fetch->settleId = g_timeout_add (fetch->scrollSeconds * 1000 + PAGE_BROWSER_SETTLE_MS, page_fetch_snapshot, fetch);
		return;
	}

	page_fetch_settle (fetch);
}

/* gives the hidden view a size and the stand-in for paused page timers */
static void
page_fetch_prepare_scrolling (WebKitWebView *view)
{
	GdkRectangle			size = { 0, 0, PAGE_BROWSER_SCROLL_WIDTH, PAGE_BROWSER_SCROLL_HEIGHT };
	g_autoptr(WebKitUserScript)	script = NULL;

	gtk_widget_measure (GTK_WIDGET (view), GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);
	gtk_widget_measure (GTK_WIDGET (view), GTK_ORIENTATION_VERTICAL, size.width, NULL, NULL, NULL, NULL);
	gtk_widget_size_allocate (GTK_WIDGET (view), &size, -1);

	/* timers of hidden pages run slower and slower, scrolling would stop after a few seconds */
	g_autoptr(WebKitFeatureList) features = webkit_settings_get_all_features ();
	for (gsize i = 0; i < webkit_feature_list_get_length (features); i++) {
		WebKitFeature *feature = webkit_feature_list_get (features, i);
		const gchar *id = webkit_feature_get_identifier (feature);

		if (g_str_equal (id, "HiddenPageDOMTimerThrottling") || g_str_equal (id, "DOMTimersThrottling"))
			webkit_settings_set_feature_enabled (webkit_web_view_get_settings (view), feature, FALSE);
	}

	script = webkit_user_script_new (page_browser_scroll_helper_js,
	                                 WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
	                                 WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
	                                 NULL, NULL);
	webkit_user_content_manager_add_script (webkit_web_view_get_user_content_manager (view), script);
}

static gboolean
on_fetch_load_failed (WebKitWebView *view, WebKitLoadEvent event, gchar *uri, GError *error, gpointer user_data)
{
	/* a stopped load (e.g. a redirect replacing it) is not an error */
	if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
		return FALSE;

	page_fetch_done ((pageFetch *)user_data, NULL, error->message, FALSE);
	return TRUE;
}

/* the page's process crashed or was stopped for using too much memory */
static void
on_fetch_terminated (WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
	debug (DEBUG_UPDATE, "page browser: page process ended (%d)", reason);
	page_fetch_done ((pageFetch *)user_data, NULL,
	                 reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT?_("The page used too much memory."):_("The page crashed while loading."),
	                 FALSE);
}

static gboolean
page_fetch_timeout (gpointer user_data)
{
	pageFetch *fetch = (pageFetch *)user_data;

	fetch->timeoutId = 0;
	page_fetch_done (fetch, NULL, _("The page did not finish loading in time."), FALSE);
	return G_SOURCE_REMOVE;
}

static gboolean
page_fetch_start (gpointer user_data)
{
	pageFetch *fetch = g_new0 (pageFetch, 1);

	fetch->job = (UpdateJob *)user_data;

	/* cancelled while queued */
	if (!fetch->job->callback) {
		update_job_finished (fetch->job);
		g_free (fetch);
		return G_SOURCE_REMOVE;
	}

	/* the page the user just passed a check on */
	{
		gchar *html = page_browser_take_offer (fetch->job->request->source);

		if (html) {
			UpdateJob *job = fetch->job;

			debug (DEBUG_UPDATE, "page browser: using the page shown after the site check for %s", job->request->source);
			g_free (fetch);
			job->result->source = g_strdup (job->request->source);
			job->result->httpstatus = 200;
			job->result->size = strlen (html);
			job->result->data = html;
			update_job_finished (job);
			return G_SOURCE_REMOVE;
		}
	}

	debug (DEBUG_UPDATE, "page browser: loading %s", fetch->job->request->source);
	if (fetch->job->request->options) {
		fetch->waitFor = g_strdup (fetch->job->request->options->waitFor);
		fetch->scrollSeconds = MIN (PAGE_BROWSER_MAX_SCROLL_SECONDS, fetch->job->request->options->scrollSeconds);
	}
	fetch->view = g_object_ref_sink (page_browser_view_new ());
	page_browser_view_make_hidden (fetch->view);
	if (fetch->scrollSeconds)
		page_fetch_prepare_scrolling (fetch->view);
	g_signal_connect (fetch->view, "load-changed", G_CALLBACK (on_fetch_load_changed), fetch);
	g_signal_connect (fetch->view, "load-failed", G_CALLBACK (on_fetch_load_failed), fetch);
	g_signal_connect (fetch->view, "web-process-terminated", G_CALLBACK (on_fetch_terminated), fetch);
	fetch->timeoutId = g_timeout_add_seconds (PAGE_BROWSER_TIMEOUT_SECONDS + fetch->scrollSeconds, page_fetch_timeout, fetch);
	webkit_web_view_load_uri (fetch->view, fetch->job->request->source);

	return G_SOURCE_REMOVE;
}

void
page_browser_process_request (UpdateJob *job)
{
	/* jobs run in worker threads, browser views live in the main loop */
	g_main_context_invoke (NULL, page_fetch_start, job);
}

/* visible window, e.g. for passing a site check */

typedef struct {
	PageBrowserPageFunc action;
	PageBrowserPickFunc pick;
	PageBrowserPageFunc closed;
	gpointer	userData;
	GDestroyNotify	destroy;
	AdwWindowTitle	*title;
	WebKitWebView	*view;
	GtkWindow	*win;
	gboolean	closing;
	gboolean	picking;
	gboolean	closeAfterAction;	/* "Check Passed": the action ends the window */
	gboolean	insisted;	/* clicked again on a page still looking like a check */
	guint		crashes;
} pageWindow;

static void
page_window_status_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
	AdwWindowTitle		*title = ADW_WINDOW_TITLE (user_data);
	g_autoptr(JSCValue)	value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), res, NULL);
	g_autofree gchar	*snapshot = value?jsc_value_to_string (value):NULL;
	gchar			*html = snapshot?strchr (snapshot, '\x01'):NULL;

	if (html)
		*html++ = 0;
	if (snapshot && page_browser_is_site_check (snapshot, html))
		adw_window_title_set_subtitle (title, _("Complete the check on the page"));
	else
		adw_window_title_set_subtitle (title, g_object_get_data (G_OBJECT (title), "check-window")
		                                      ?_("Passed? Click \"Check Passed\" and the feed reads this page")
		                                      :_("Done? Close this window and the feed updates"));
	g_object_unref (title);
}

static void
on_window_load_changed (WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
	pageWindow *window = (pageWindow *)user_data;

	if (WEBKIT_LOAD_FINISHED == event)
		webkit_web_view_evaluate_javascript (view, PAGE_BROWSER_SNAPSHOT_JS, -1, NULL, NULL, NULL,
		                                     page_window_status_cb, g_object_ref (window->title));
}

/* the page as shown goes to the closed callback (NULL if it is still a
   check page), then the window goes */
static void
page_window_closing_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
	pageWindow		*window = (pageWindow *)user_data;
	g_autoptr(JSCValue)	value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), res, NULL);
	g_autofree gchar	*snapshot = value?jsc_value_to_string (value):NULL;
	gchar			*html = snapshot?strchr (snapshot, '\x01'):NULL;

	if (html)
		*html++ = 0;
	if (html && page_browser_is_site_check (snapshot, html))
		html = NULL;

	if (window->closed)
		window->closed (window->userData, webkit_web_view_get_uri (window->view), html);
	if (window->destroy)
		window->destroy (window->userData);

	gtk_window_destroy (window->win);
	g_free (window);
}

/* "Pick an Item": outlines the repeating block under the mouse and on
   click posts selectors for it. Of the ancestors with a selector (tag
   with class, or a custom element's tag like "shop-item-card")
   shared by at least 3 elements most of which hold a link, the block is
   the outermost one repeated about as often as the most repeated one: a
   shop tile rather than its title box (as often) or a grid row (less
   often). Bits repeated inside each block without links (labels, badges)
   do not count, they made the picker choose them on video sites. The
   arrow keys pick a bigger or smaller block, a box at the bottom says
   which and how many. Looks into open shadow roots (web components), and
   holds back the page's own click handling while picking (sites
   navigating on mouse down). Inside the block, title, text, price and
   picture are guessed by class names and ids; a panel on the page then
   shows what each would read and lets the user click the right part
   instead ("Choose"), or leave it out ("None"). */
static const gchar *pickScript =
	"(function () {\n"
	" if (window.__lifereaPick) return; window.__lifereaPick = true;\n"
	" var plain = /^[A-Za-z_][A-Za-z0-9_-]*$/, shown = null, saved = '', cands = [], level = 0, hint = null;\n"
	" var events = ['mouseover', 'click', 'mousedown', 'mouseup', 'pointerdown', 'pointerup', 'auxclick', 'dblclick'];\n"
	" /* after the item is clicked: which part is what (see parts ()) */\n"
	" var chosen = null, choosing = null, part = null, partSaved = '', host = null, panel = null;\n"
	" function up (el) { return el.parentElement || (el.parentNode && el.parentNode.host) || null; }\n"
	" var roots = null, rootsTime = 0;\n"
	" function collect (r, list) { var all = r.querySelectorAll ('*');\n"
	"  for (var i = 0; i < all.length; i++) if (all[i].shadowRoot) { list.push (all[i].shadowRoot); collect (all[i].shadowRoot, list); } }\n"
	" function shadowRoots () { if (!roots || Date.now () - rootsTime > 2000) { roots = []; collect (document, roots); rootsTime = Date.now (); } return roots; }\n"
	" function find (el, s) {\n"
	"  var out = [], r = shadowRoots ();\n"
	"  try { out = Array.prototype.slice.call (document.querySelectorAll (s));\n"
	"   for (var i = 0; i < r.length; i++) out = out.concat (Array.prototype.slice.call (r[i].querySelectorAll (s))); } catch (e) {}\n"
	"  return out; }\n"
	" function deep (el, s) {\n"
	"  var out = [], todo = [el];\n"
	"  while (todo.length) { var x = todo.shift (), all;\n"
	"   try { all = x.querySelectorAll (s); } catch (e) { all = []; }\n"
	"   for (var i = 0; i < all.length; i++) out.push (all[i]);\n"
	"   if (x.shadowRoot) todo.push (x.shadowRoot);\n"
	"   var hosts = x.querySelectorAll ('*'); for (var j = 0; j < hosts.length; j++) if (hosts[j].shadowRoot) todo.push (hosts[j].shadowRoot); }\n"
	"  return out; }\n"
	" function linked (e) { return e.matches ('a[href]') || deep (e, 'a[href]').length > 0; }\n"
	" function best (el) {\n"
	"  var tag = el.tagName.toLowerCase (), list = [], pick = null;\n"
	"  if (tag.indexOf ('-') > 0) list.push (tag);\n"
	"  for (var i = 0; i < el.classList.length; i++) if (plain.test (el.classList[i])) list.push (tag + '.' + el.classList[i]);\n"
	"  for (var j = 0; j < list.length; j++) {\n"
	"   var m = find (el, list[j]), n = m.length, step, k, hits = 0, looked = 0;\n"
	"   if (n < 3 || (pick && n >= pick.n)) continue;\n"
	"   step = Math.max (1, Math.floor (n / 30));\n"
	"   for (k = 0; k < n; k += step) { looked++; if (linked (m[k])) hits++; }\n"
	"   if (hits * 10 >= looked * 6) pick = { s: list[j], n: n };\n"
	"  }\n"
	"  return pick; }\n"
	" function block (el) {\n"
	"  var found = [], most = 0, at = -1;\n"
	"  for (var a = el; a && a !== document.body && a !== document.documentElement; a = up (a)) {\n"
	"   var b = best (a);\n"
	"   if (b) { found.push ({ el: a, sel: b.s, n: b.n }); most = Math.max (most, b.n); } }\n"
	"  for (var i = 0; i < found.length; i++) if (found[i].n * 5 >= most * 4) at = i;\n"
	"  cands = found; level = at; }\n"
	" /* whole words of a name (\"shopItemCardTitle\", \"item__price\"),\n"
	"    so \"desc\" finds \"description\" but not \"modeScale\" */\n"
	" function named (c, word) {\n"
	"  var parts = c.replace (/([a-z0-9])([A-Z])/g, '$1 $2').toLowerCase ().split (/[^a-z0-9]+/);\n"
	"  for (var i = 0; i < parts.length; i++) if (parts[i].indexOf (word) === 0) return true;\n"
	"  return false; }\n"
	" /* first word first: \"title\" beats \"heading\"; text must not hold the title */\n"
	" function inner (el, words, avoid) {\n"
	"  var all = deep (el, '[class], [id]');\n"
	"  for (var w = 0; w < words.length; w++) for (var i = 0; i < all.length; i++) {\n"
	"   var names = [], e = all[i];\n"
	"   if (avoid && e.contains (avoid)) continue;\n"
	"   for (var j = 0; j < e.classList.length; j++) names.push ('.' + e.classList[j]);\n"
	"   if (e.id) names.push ('#' + e.id);\n"
	"   for (var k = 0; k < names.length; k++) {\n"
	"    var c = names[k].substring (1);\n"
	"    if (plain.test (c) && named (c, words[w])) return { s: names[k], el: e }; } }\n"
	"  return null; }\n"
	" function heading (el) { var h = deep (el, 'h1, h2, h3, h4, h5, h6')[0]; return h ? { s: h.tagName.toLowerCase (), el: h } : null; }\n"
	" function outline (el) {\n"
	"  if (shown) shown.style.outline = saved; shown = el;\n"
	"  if (el) { saved = el.style.outline; el.style.outline = '3px solid #3584e4'; } }\n"
	" function show () {\n"
	"  var c = level >= 0 ? cands[level] : null;\n"
	"  outline (c ? c.el : null);\n"
	"  if (!hint) {\n"
	"   hint = document.createElement ('div');\n"
	"   hint.style.cssText = 'position:fixed;left:12px;bottom:12px;z-index:2147483647;pointer-events:none;'\n"
	"    + 'background:#1e1e1e;color:#fff;font:13px sans-serif;padding:8px 12px;border-radius:8px;'\n"
	"    + 'box-shadow:0 2px 8px rgba(0,0,0,.4);max-width:70%';\n"
	"   document.documentElement.appendChild (hint); }\n"
	"  hint.textContent = c ? (c.sel + ': ' + c.n + ' like it on the page. Up/Down arrow: bigger/smaller block, click: pick, Esc: stop')\n"
	"                       : 'Point at one item (e.g. a video or shop tile). Esc: stop'; }\n"
	" function target (e) { var t = e.composedPath ? e.composedPath ()[0] : e.target; while (t && t.nodeType !== 1) t = t.parentNode; return t; }\n"
	" function inPanel (e) { return host && e.composedPath && e.composedPath ().indexOf (host) >= 0; }\n"
	" function mark (el) {\n"
	"  if (part) part.style.outline = partSaved; part = el;\n"
	"  if (el) { partSaved = el.style.outline; el.style.outline = '2px dashed #e66100'; } }\n"
	" function stop () {\n"
	"  mark (null); outline (null); if (hint) hint.remove (); hint = null; if (host) host.remove (); host = null;\n"
	"  window.__lifereaPick = false;\n"
	"  events.forEach (function (n) { window.removeEventListener (n, handle, true); });\n"
	"  window.removeEventListener ('keydown', key, true); }\n"
	" function post (message) { stop (); window.webkit.messageHandlers.lifereaPick.postMessage (message); }\n"
	"\n"
	" /* parts: the user says by clicking which part of the item is the\n"
	"    title, text, price and picture; selectors relative to the item,\n"
	"    preferring one most other items have too */\n"
	" var fields = [ ['title', 'Title'], ['text', 'Text'], ['price', 'Price'], ['image', 'Picture'] ];\n"
	" function within (el) { var all = deep (chosen.el, '*'); return all.indexOf (el) >= 0; }\n"
	" function relative (el, field) {\n"
	"  if (field === 'image' && el.tagName !== 'IMG') { var img = deep (el, 'img')[0]; if (!img) return null; el = img; }\n"
	"  var list = [], tag = el.tagName.toLowerCase (), blocks = find (document, chosen.sel), step = Math.max (1, Math.floor (blocks.length / 30));\n"
	"  for (var i = 0; i < el.classList.length; i++) if (plain.test (el.classList[i])) list.push ('.' + el.classList[i]);\n"
	"  if (el.id && plain.test (el.id)) list.push ('#' + el.id);\n"
	"  list.push (tag);\n"
	"  var fallback = null;\n"
	"  for (var j = 0; j < list.length; j++) {\n"
	"   var first = deep (chosen.el, list[j])[0] === el, have = 0, looked = 0;\n"
	"   for (var k = 0; k < blocks.length; k += step) { looked++; if (deep (blocks[k], list[j]).length) have++; }\n"
	"   var common = have * 10 >= looked * 6;\n"
	"   if (first && common) return list[j];\n"
	"   if (!fallback && (common || first)) fallback = list[j]; }\n"
	"  return fallback || tag; }\n"
	" function sample (field) {\n"
	"  var s = chosen[field]; if (!s) return '(none)';\n"
	"  var el = deep (chosen.el, s)[0]; if (!el) return '(not in this item)';\n"
	"  if (field === 'image') { var src = el.getAttribute ('data-src') || el.getAttribute ('src') || ''; return src ? 'picture: ' + src.split ('/').pop ().substring (0, 50) : 'picture (loads when scrolled to)'; }\n"
	"  var t = (el.textContent || '').replace (/\\s+/g, ' ').trim ();\n"
	"  return t ? (t.length > 80 ? t.substring (0, 80) + '...' : t) : '(empty)'; }\n"
	" function button (label, primary, onclick) {\n"
	"  var b = document.createElement ('button'); b.textContent = label; if (primary) b.className = 'primary';\n"
	"  b.addEventListener ('click', function (e) { e.preventDefault (); e.stopPropagation (); onclick (); });\n"
	"  return b; }\n"
	" function draw () {\n"
	"  while (panel.firstChild) panel.removeChild (panel.firstChild);\n"
	"  var head = document.createElement ('div'); head.className = 'head';\n"
	"  head.textContent = choosing ? ('Click the ' + fields.filter (function (f) { return f[0] === choosing; })[0][1].toLowerCase () + ' inside the outlined item (Esc: back)')\n"
	"                              : ('Item: ' + chosen.sel + ' (' + chosen.n + ' on the page). Check each part, Choose to click it yourself.');\n"
	"  panel.appendChild (head);\n"
	"  fields.forEach (function (f) {\n"
	"   var row = document.createElement ('div'), name = document.createElement ('b'), value = document.createElement ('span');\n"
	"   row.className = 'row' + (choosing === f[0] ? ' active' : ''); name.textContent = f[1];\n"
	"   value.textContent = sample (f[0]); value.title = chosen[f[0]] || '';\n"
	"   row.appendChild (name); row.appendChild (value);\n"
	"   row.appendChild (button ('Choose', false, function () { choosing = f[0]; draw (); }));\n"
	"   row.appendChild (button ('None', false, function () { chosen[f[0]] = ''; choosing = null; draw (); }));\n"
	"   panel.appendChild (row); });\n"
	"  var foot = document.createElement ('div'); foot.className = 'foot';\n"
	"  foot.appendChild (button ('Other item', false, function () { mark (null); chosen = null; choosing = null; host.remove (); host = null; show (); }));\n"
	"  foot.appendChild (button ('Cancel', false, function () { post ('cancel'); }));\n"
	"  foot.appendChild (button ('Use these', true, function () {\n"
	"   post (JSON.stringify ({ item: chosen.sel, title: chosen.title || 'a', text: chosen.text, price: chosen.price, image: chosen.image })); }));\n"
	"  panel.appendChild (foot); }\n"
	" function parts (b) {\n"
	"  var title = inner (b.el, ['title', 'name', 'heading']) || heading (b.el);\n"
	"  var text = inner (b.el, ['date', 'summary', 'desc', 'metadata'], title ? title.el : null), price = inner (b.el, ['price', 'cost']);\n"
	"  chosen = { el: b.el, sel: b.sel, n: b.n, title: title ? title.s : 'a', text: text ? text.s : '', price: price ? price.s : '',\n"
	"             image: deep (b.el, 'img').length ? 'img' : '' };\n"
	"  if (hint) hint.remove (); hint = null;\n"
	"  host = document.createElement ('div');\n"
	"  host.style.cssText = 'position:fixed;right:12px;bottom:12px;z-index:2147483647';\n"
	"  var root = host.attachShadow ({ mode: 'closed' }), style = document.createElement ('style');\n"
	"  style.textContent = ':host{all:initial}div.p{font:13px sans-serif;color:#fff;background:#1e1e1e;border-radius:10px;padding:10px 12px;'\n"
	"   + 'box-shadow:0 2px 10px rgba(0,0,0,.5);width:460px;max-width:90vw}.head{margin-bottom:8px;color:#ccc}'\n"
	"   + '.row{display:flex;gap:8px;align-items:center;padding:4px 0;border-top:1px solid #333}.row.active{background:#3a2a10}'\n"
	"   + '.row b{width:56px;flex:none}.row span{flex:1;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;color:#9cf}'\n"
	"   + 'button{font:12px sans-serif;background:#333;color:#fff;border:1px solid #555;border-radius:6px;padding:3px 8px;cursor:pointer}'\n"
	"   + 'button.primary{background:#3584e4;border-color:#3584e4}.foot{display:flex;gap:8px;justify-content:flex-end;margin-top:8px}';\n"
	"  panel = document.createElement ('div'); panel.className = 'p';\n"
	"  root.appendChild (style); root.appendChild (panel);\n"
	"  document.documentElement.appendChild (host);\n"
	"  draw (); }\n"
	"\n"
	" function handle (e) {\n"
	"  if (inPanel (e)) return;\n"
	"  var t = target (e);\n"
	"  if (chosen) {\n"
	"   if (e.type === 'mouseover') { mark (choosing && t && within (t) ? t : null); return; }\n"
	"   e.preventDefault (); e.stopImmediatePropagation ();\n"
	"   if (e.type === 'click' && choosing && t && within (t)) {\n"
	"    var s = relative (t, choosing);\n"
	"    if (s) chosen[choosing] = s;\n"
	"    choosing = null; mark (null); draw (); }\n"
	"   return; }\n"
	"  if (e.type === 'mouseover') { if (t) { block (t); show (); } return; }\n"
	"  e.preventDefault (); e.stopImmediatePropagation ();\n"
	"  if (e.type === 'click' && level >= 0) parts (cands[level]); }\n"
	" function key (e) {\n"
	"  if (e.key === 'Escape') {\n"
	"   e.preventDefault (); e.stopImmediatePropagation ();\n"
	"   if (chosen && choosing) { choosing = null; mark (null); draw (); } else post ('cancel');\n"
	"   return; }\n"
	"  if (chosen) return;\n"
	"  if (e.key === 'ArrowUp' && level >= 0 && level < cands.length - 1) level++;\n"
	"  else if (e.key === 'ArrowDown' && level > 0) level--;\n"
	"  else return;\n"
	"  e.preventDefault (); e.stopImmediatePropagation (); show (); }\n"
	" events.forEach (function (n) { window.addEventListener (n, handle, true); });\n"
	" window.addEventListener ('keydown', key, true);\n"
	" show ();\n"
	"}) ()\n";

typedef struct {
	gpointer	window;
	gchar		*json;
} pagePick;

/* the page as shown, together with the picked selectors */
static void
page_window_pick_snapshot_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
	pagePick		*pick = (pagePick *)user_data;
	pageWindow		*window = (pageWindow *)pick->window;
	g_autoptr(JSCValue)	value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), res, NULL);
	g_autofree gchar	*snapshot = value?jsc_value_to_string (value):NULL;
	gchar			*html = snapshot?strchr (snapshot, '\x01'):NULL;
	g_autoptr(JsonParser)	parser = json_parser_new ();

	if (!window->closing && html && json_parser_load_from_data (parser, pick->json, -1, NULL) &&
	    JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))) {
		JsonObject *obj = json_node_get_object (json_parser_get_root (parser));

		*html++ = 0;
		adw_window_title_set_subtitle (window->title, _("Item picked, see the preview in the feed dialog"));
		window->pick (window->userData, webkit_web_view_get_uri (window->view), html,
		              json_object_get_string_member_with_default (obj, "item", NULL),
		              json_object_get_string_member_with_default (obj, "title", NULL),
		              json_object_get_string_member_with_default (obj, "image", NULL),
		              json_object_get_string_member_with_default (obj, "text", NULL),
		              json_object_get_string_member_with_default (obj, "price", NULL));
	}

	g_free (pick->json);
	g_free (pick);
}

static void
on_pick_message (WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
	pageWindow		*window = (pageWindow *)user_data;
	g_autofree gchar	*message = jsc_value_to_string (value);
	pagePick		*pick;

	if (window->closing || !window->picking)
		return;
	window->picking = FALSE;

	if (g_str_equal (message, "cancel")) {
		adw_window_title_set_subtitle (window->title, NULL);
		return;
	}

	pick = g_new0 (pagePick, 1);
	pick->window = window;
	pick->json = g_steal_pointer (&message);
	webkit_web_view_evaluate_javascript (window->view, PAGE_BROWSER_SNAPSHOT_JS, -1, NULL, NULL, NULL,
	                                     page_window_pick_snapshot_cb, pick);
}

static void
on_window_pick_clicked (GtkButton *button, gpointer user_data)
{
	pageWindow *window = (pageWindow *)user_data;

	window->picking = TRUE;
	adw_window_title_set_subtitle (window->title, _("Click one item on the page (Esc cancels)"));
	webkit_web_view_evaluate_javascript (window->view, pickScript, -1, NULL, NULL, NULL, NULL, NULL);
}

/* "Detect Items Here": the page as shown right now */
static void
page_window_action_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
	pageWindow		*window = (pageWindow *)user_data;
	g_autoptr(JSCValue)	value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), res, NULL);
	g_autofree gchar	*snapshot = value?jsc_value_to_string (value):NULL;
	gchar			*html = snapshot?strchr (snapshot, '\x01'):NULL;

	if (window->closing)
		return;
	if (html)
		*html++ = 0;
	if (!html) {
		adw_window_title_set_subtitle (window->title, _("The page could not be read, wait until it has loaded"));
		return;
	}
	if (page_browser_is_site_check (snapshot, html)) {
		/* the user sees the page: a second click trusts them */
		if (!window->closeAfterAction || !window->insisted) {
			window->insisted = TRUE;
			adw_window_title_set_subtitle (window->title, window->closeAfterAction
			                               ?_("This still looks like a check page. Passed it anyway? Click again")
			                               :_("Complete the check on the page first"));
			return;
		}
	}

	if (window->closeAfterAction) {
		window->action (window->userData, webkit_web_view_get_uri (window->view), html);
		window->closed = NULL;	/* done, closing changes nothing more */
		gtk_window_close (window->win);
		return;
	}

	adw_window_title_set_subtitle (window->title, _("Items handed to the feed dialog, see its preview"));
	window->action (window->userData, webkit_web_view_get_uri (window->view), html);
}

static void
on_window_action_clicked (GtkButton *button, gpointer user_data)
{
	pageWindow *window = (pageWindow *)user_data;

	webkit_web_view_evaluate_javascript (window->view, PAGE_BROWSER_SNAPSHOT_JS, -1, NULL, NULL, NULL,
	                                     page_window_action_cb, window);
}

/* Heavy pages (e.g. video sites) can crash the page's process or run
   out of memory; load them again instead of leaving a blank window */
static void
on_window_terminated (WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
	pageWindow *window = (pageWindow *)user_data;

	debug (DEBUG_GUI, "page browser window: page process ended (%d)", reason);
	if (window->closing)
		return;
	window->picking = FALSE;

	if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
		return;

	if (++window->crashes <= 2) {
		adw_window_title_set_subtitle (window->title, _("The page crashed, loading it again..."));
		webkit_web_view_reload (view);
	} else {
		adw_window_title_set_subtitle (window->title, _("The page keeps crashing, try again later"));
	}
}

static gboolean
on_window_close_request (GtkWindow *gtkWindow, gpointer user_data)
{
	pageWindow *window = (pageWindow *)user_data;

	if (window->closing)
		return TRUE;
	window->closing = TRUE;

	g_signal_handlers_disconnect_by_func (window->view, on_window_load_changed, window);
	g_signal_handlers_disconnect_by_func (window->view, on_window_terminated, window);
	g_signal_handlers_disconnect_by_data (webkit_web_view_get_user_content_manager (window->view), window);
	gtk_widget_set_visible (GTK_WIDGET (gtkWindow), FALSE);
	webkit_web_view_evaluate_javascript (window->view, PAGE_BROWSER_SNAPSHOT_JS, -1, NULL, NULL, NULL,
	                                     page_window_closing_cb, window);

	return TRUE;	/* closed once the page was read */
}

static pageWindow *
page_browser_window_new (const gchar *url, const gchar *actionLabel, PageBrowserPageFunc action,
                         PageBrowserPickFunc pick, PageBrowserPageFunc closed,
                         gpointer user_data, GDestroyNotify destroy, gboolean closeAfterAction);

void
page_browser_open_window (const gchar *url, const gchar *actionLabel, PageBrowserPageFunc action,
                          PageBrowserPickFunc pick, PageBrowserPageFunc closed,
                          gpointer user_data, GDestroyNotify destroy)
{
	page_browser_window_new (url, actionLabel, action, pick, closed, user_data, destroy, FALSE);
}

void
page_browser_open_check_window (const gchar *url, PageBrowserPageFunc passed, PageBrowserPageFunc closed,
                                gpointer user_data, GDestroyNotify destroy)
{
	page_browser_window_new (url, _("Check _Passed"), passed, NULL, closed, user_data, destroy, TRUE);
}

static pageWindow *
page_browser_window_new (const gchar *url, const gchar *actionLabel, PageBrowserPageFunc action,
                         PageBrowserPickFunc pick, PageBrowserPageFunc closed,
                         gpointer user_data, GDestroyNotify destroy, gboolean closeAfterAction)
{
	GtkWidget	*win = adw_window_new ();
	GtkWidget	*toolbar = adw_toolbar_view_new ();
	GtkWidget	*header = adw_header_bar_new ();
	WebKitWebView	*view = page_browser_view_new ();
	pageWindow	*window = g_new0 (pageWindow, 1);

	window->action = action;
	window->pick = pick;
	window->closed = closed;
	window->userData = user_data;
	window->destroy = destroy;
	window->closeAfterAction = closeAfterAction;
	window->view = view;
	window->win = GTK_WINDOW (win);
	window->title = ADW_WINDOW_TITLE (adw_window_title_new (_("Site Check"), _("Loading...")));
	if (closeAfterAction)
		g_object_set_data (G_OBJECT (window->title), "check-window", GINT_TO_POINTER (1));

	adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (window->title));
	if (actionLabel && action) {
		GtkWidget *button = gtk_button_new_with_mnemonic (actionLabel);
		gtk_widget_add_css_class (button, "suggested-action");
		if (closeAfterAction)
			gtk_widget_set_tooltip_text (button, _("Click once the page shows the site instead of the check: the feed reads this page and its padlock goes"));
		g_signal_connect (button, "clicked", G_CALLBACK (on_window_action_clicked), window);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), button);
	}
	if (pick) {
		GtkWidget *button = gtk_button_new_with_mnemonic (_("_Pick an Item"));
		WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager (view);

		gtk_widget_set_tooltip_text (button, _("Click one of the items (e.g. a shop tile) on the page, the feed uses all items like it"));
		g_signal_connect (button, "clicked", G_CALLBACK (on_window_pick_clicked), window);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), button);

		g_signal_connect (manager, "script-message-received::lifereaPick", G_CALLBACK (on_pick_message), window);
		webkit_user_content_manager_register_script_message_handler (manager, "lifereaPick", NULL);
	}
	adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
	adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (view));
	adw_window_set_content (ADW_WINDOW (win), toolbar);

	/* Own window group: modal dialogs (e.g. the page feed dialog this
	   window is opened from) block input to all windows of their group,
	   which made this window unusable */
	{
		GtkWindowGroup *group = gtk_window_group_new ();
		gtk_window_group_add_window (group, GTK_WINDOW (win));
		g_object_unref (group);
	}

	gtk_window_set_default_size (GTK_WINDOW (win), 1000, 750);
	gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (liferea_shell_get_window ()));
	gtk_window_set_title (GTK_WINDOW (win), _("Site Check"));

	g_signal_connect (view, "load-changed", G_CALLBACK (on_window_load_changed), window);
	g_signal_connect (view, "web-process-terminated", G_CALLBACK (on_window_terminated), window);
	g_signal_connect (win, "close-request", G_CALLBACK (on_window_close_request), window);

	webkit_web_view_load_uri (view, url);
	gtk_window_present (GTK_WINDOW (win));

	return window;
}
