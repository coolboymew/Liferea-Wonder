/**
 * @file feed_parser.c  parsing of different feed formats
 *
 * Copyright (C) 2008-2026 Lars Windolf <lars.windolf@gmx.de>
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

#include <string.h>

#include "common.h"
#include "debug.h"
#include "feedlist.h"
#include "html.h"
#include "metadata.h"
#include "node.h"
#include "update.h"
#include "xml.h"
#include "parsers/atom10.h"
#include "parsers/html5_feed.h"
#include "parsers/ldjson_feed.h"
#include "parsers/rss_channel.h"
#include "parsers/gopher.h"
#include "parsers/page_scraper.h"

#define AUTO_DISCOVERY_MAX_REDIRECTS	5

static GSList *feedHandlers = NULL;	/**< list of available parser implementations */

struct feed_type {
	gint id_num;
	gchar *id_str;
};

static void
feed_parsers_cleanup (void)
{
	g_slist_free_full (feedHandlers, (GDestroyNotify)g_free);
	feedHandlers = NULL;
}

static GSList *
feed_parsers_get_list (void)
{
	if (feedHandlers)
		return feedHandlers;

	feedHandlers = g_slist_append (feedHandlers, rss_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, atom10_init_feed_handler ());

	/* Order is important ! */
	feedHandlers = g_slist_append (feedHandlers, ldjson_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, html5_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, gopher_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, page_scraper_init_feed_handler ());

	atexit(feed_parsers_cleanup);

	return feedHandlers;
}

const gchar *
feed_type_fhp_to_str (feedHandlerPtr fhp)
{
	if (!fhp)
		return NULL;
	return fhp->typeStr;
}

feedHandlerPtr
feed_type_str_to_fhp (const gchar *str)
{
	GSList *iter;
	feedHandlerPtr fhp = NULL;

	if (!str)
		return NULL;

	for (iter = feed_parsers_get_list (); iter != NULL; iter = iter->next) {
		fhp = (feedHandlerPtr)iter->data;
		if (!strcmp(str, fhp->typeStr))
			return fhp;
	}

	return NULL;
}

feedParserCtxtPtr
feed_parser_ctxt_new (subscriptionPtr subscription, const gchar *data, gsize size)
{
	feedParserCtxtPtr ctxt;

	ctxt = g_new0 (struct feedParserCtxt, 1);
	ctxt->subscription = subscription;
	ctxt->data = data;
	ctxt->dataLength = size;
	ctxt->origSubscriptionMetadata = metadata_list_copy (subscription->metadata);

	return ctxt;
}

void
feed_parser_ctxt_free (feedParserCtxtPtr ctxt)
{
	if (ctxt) {
		/* Don't free the itemset! */
		metadata_list_free (ctxt->origSubscriptionMetadata);
		g_free (ctxt->title);
		g_free (ctxt);
	}
}

/* Common feed addresses tried as a last resort when a page has neither
   a feed link tag nor a feed-like link, relative to the page's folder
   and to the site root (WordPress, Blogger, Hugo, Jekyll, ...). */
static const gchar *guessedFeedPaths[] = {
	"feed", "rss", "feed.xml", "rss.xml", "atom.xml", "index.xml", "feeds/posts/default", NULL
};

static void feed_parser_guess_next (subscriptionPtr subscription);

feedHandlerPtr
feed_parser_probe (const gchar *source, const gchar *data, gsize size, gchar **title)
{
	Node			*node = node_new ("feed");
	subscriptionPtr		subscription = subscription_new (source, NULL, NULL);
	feedParserCtxtPtr	ctxt;
	feedHandlerPtr		handler = NULL;

	/* A throw-away node and subscription (some parsers set the node
	   title directly). Auto discovery is disabled as it would start
	   real subscription updates. */
	subscription->autoDiscoveryTries = G_MAXUINT / 2;
	subscription->node = node;
	node->subscription = subscription;

	ctxt = feed_parser_ctxt_new (subscription, data, size);
	if (feed_parse (ctxt) && subscription->fhp) {
		handler = subscription->fhp;
		if (title)
			*title = g_strdup (ctxt->title?ctxt->title:node_get_title (node));
	}
	g_list_free_full (ctxt->items, g_object_unref);
	ctxt->items = NULL;
	feed_parser_ctxt_free (ctxt);
	g_object_unref (node);	/* also frees the subscription */

	return handler;
}

static gboolean
feed_parser_guess_cb (UpdateJob *job)
{
	subscriptionPtr	subscription = (subscriptionPtr)job->user_data;
	feedHandlerPtr	handler = NULL;

	if (job->result->data && job->result->httpstatus >= 200 && job->result->httpstatus < 300)
		handler = feed_parser_probe (job->request->source, job->result->data, job->result->size, NULL);

	/* HTML extractors would accept any page, we want a real feed */
	if (handler && !handler->html) {
		debug (DEBUG_UPDATE, "Guessed feed address works: %s", job->request->source);
		g_slist_free_full (subscription->discoveryCandidates, g_free);
		subscription->discoveryCandidates = NULL;
		subscription_set_source (subscription, job->request->source);
		subscription_update (subscription, UPDATE_REQUEST_RESET_TITLE | UPDATE_REQUEST_PRIORITY_HIGH);
	} else {
		feed_parser_guess_next (subscription);
	}

	return TRUE;
}

static void
feed_parser_guess_next (subscriptionPtr subscription)
{
	g_autofree gchar	*url = NULL;
	UpdateRequest		*request;

	if (!subscription->discoveryCandidates) {
		debug (DEBUG_UPDATE, "None of the common feed addresses worked for %s", subscription_get_source (subscription));
		subscription->error = FETCH_ERROR_DISCOVER;
		if (subscription->node) {
			subscription->node->available = FALSE;
			feedlist_node_was_updated (subscription->node);
		}
		return;
	}

	url = subscription->discoveryCandidates->data;
	subscription->discoveryCandidates = g_slist_delete_link (subscription->discoveryCandidates, subscription->discoveryCandidates);

	debug (DEBUG_UPDATE, "Trying common feed address %s", url);
	/* no subscription options, as for the blogroll (see Github #678) */
	request = update_request_new ("GET", url, NULL, NULL);
	update_job_new (subscription, request, feed_parser_guess_cb, subscription, UPDATE_REQUEST_NO_FEED);
}

/* Starts trying common feed addresses, returns FALSE if there are none to try */
static gboolean
feed_parser_guess_start (subscriptionPtr subscription)
{
	const gchar	*page = subscription_get_source (subscription);
	const gchar	*bases[] = { "", "/", NULL };

	if (subscription->discoveryGuessed || !page ||
	    !(g_str_has_prefix (page, "http://") || g_str_has_prefix (page, "https://")))
		return FALSE;

	subscription->discoveryGuessed = TRUE;

	/* "" resolves against the page's folder, "/" against the site root */
	for (guint b = 0; bases[b]; b++) {
		for (guint p = 0; guessedFeedPaths[p]; p++) {
			g_autofree gchar *relative = g_strconcat (bases[b], guessedFeedPaths[p], NULL);
			gchar *url = g_uri_resolve_relative (page, relative, G_URI_FLAGS_NONE, NULL);

			if (url && !g_str_equal (url, page) &&
			    !g_slist_find_custom (subscription->discoveryCandidates, url, (GCompareFunc)g_strcmp0))
				subscription->discoveryCandidates = g_slist_append (subscription->discoveryCandidates, url);
			else
				g_free (url);
		}
	}

	debug (DEBUG_UPDATE, "No feed link on %s, trying %u common feed addresses", page, g_slist_length (subscription->discoveryCandidates));
	feed_parser_guess_next (subscription);
	return TRUE;
}

/**
 * This function tries to find a feed link for a given HTTP URI. It
 * tries to download it. If it finds a valid feed source it parses
 * this source instead into the given feed parsing context. It also
 * replaces the HTTP URI with the found feed source.
 */
static gboolean
feed_parser_auto_discover (feedParserCtxtPtr ctxt)
{
	g_autofree gchar *blogroll = NULL;

	blogroll = html_auto_discover_blogroll (ctxt->data, subscription_get_source (ctxt->subscription));
	if (blogroll) {
		debug (DEBUG_UPDATE, "Discovered blogroll: %s", blogroll);
		metadata_list_set (&(ctxt->subscription->metadata), "blogroll", blogroll);
	}

	gchar	*source = NULL;
	GSList	*links;

	debug (DEBUG_UPDATE, "Starting feed auto discovery (%s) redirects=%d", subscription_get_source (ctxt->subscription), ctxt->subscription->autoDiscoveryTries);

	links = html_auto_discover_feed (ctxt->data, subscription_get_source (ctxt->subscription));
	if (links)
		source = links->data;	// FIXME: let user choose feed!

	/* FIXME: we only need the !g_str_equal as a workaround after a 404 */
	if (source && !g_str_equal (source, subscription_get_source (ctxt->subscription))) {
		debug (DEBUG_UPDATE, "Discovered link: %s", source);
		subscription_set_source (ctxt->subscription, source);

		/* The feed that was processed wasn't the correct one, we need to redownload it.
		 * Cancel the update in case there's one in progress */
		subscription_cancel_update (ctxt->subscription);
		subscription_update (ctxt->subscription, UPDATE_REQUEST_RESET_TITLE);
		g_free (source);

		return TRUE;
	}

	debug (DEBUG_UPDATE, "No feed link found!");

	/* last resort: try common feed addresses (asynchronously) */
	return feed_parser_guess_start (ctxt->subscription);
}

static void
feed_parser_ctxt_cleanup (feedParserCtxtPtr ctxt)
{
	/* different parsing attempts might modify the subscription metadata
	   so we reload it each time from DB */
	metadata_list_free (ctxt->subscription->metadata);
	ctxt->subscription->metadata = metadata_list_copy (ctxt->origSubscriptionMetadata);
}

/**
 * General feed source parsing function. Parses the passed feed source
 * and tries to determine the source type. If all feed handlers fail
 * tries to do HTML5 feed extraction. If this also fails starts feed
 * link auto-discovery.
 *
 * @param ctxt		feed parsing context
 *
 * @returns FALSE if auto discovery is indicated,
 *          TRUE if feed type was recognized and parsing was successful
 */
gboolean
feed_parse (feedParserCtxtPtr ctxt)
{
	xmlNodePtr	xmlNode = NULL, htmlNode = NULL;
	xmlDocPtr	xmlDoc, htmlDoc;
	GSList		*handlerIter;
	gboolean	autoDiscovery = FALSE, success = FALSE;

	g_assert (NULL == ctxt->items);

	if (ctxt->subscription->parseErrors)
		g_string_truncate (ctxt->subscription->parseErrors, 0);
	else
		ctxt->subscription->parseErrors = g_string_new (NULL);

	/* page feeds: the user's selectors instead of format detection */
	if (ctxt->subscription->scraper) {
		feed_parser_ctxt_cleanup (ctxt);
		return page_scraper_parse (ctxt);
	}

	/* Prepare two documents, one parse as XML and one as XHTML.
	   Depending on the feed parser being an HTML parser the one
	   or the other is used. */

	/* 1.) try to parse downloaded data as XML */
	do {
		if (NULL == (xmlDoc = xml_parse_feed (ctxt))) {
			ctxt->subscription->error = FETCH_ERROR_XML;
			break;
		}

		if (NULL == (xmlNode = xmlDocGetRootElement (xmlDoc))) {
			ctxt->subscription->error = FETCH_ERROR_XML;
			g_string_append (ctxt->subscription->parseErrors, _("Empty document!"));
			break;
		}

		while (xmlNode && xmlIsBlankNode (xmlNode)) {
			xmlNode = xmlNode->next;
		}

		if (!xmlNode->name) {
			g_string_append (ctxt->subscription->parseErrors, _("Invalid XML!"));
			break;
		}
	} while (0);

	/* 2.) also prepare data as XHTML */
	do {
		if (NULL == (htmlDoc = xhtml_parse (ctxt->data, ctxt->dataLength)))
			break;

		if (NULL == (htmlNode = xmlDocGetRootElement (htmlDoc))) {
			//g_string_append (ctxt->subscription->parseErrors, _("Empty document!"));
			break;
		}
	} while (0);

	/* 3.) try all non-HTML parsers (this are all syndication format parsers) */
	handlerIter = feed_parsers_get_list ();
	while (handlerIter) {
		feedHandlerPtr handler = (feedHandlerPtr)(handlerIter->data);

		// XML detection
		if (xmlNode && handler->checkFormat && !handler->html && (*(handler->checkFormat))(xmlDoc, xmlNode)) {
			ctxt->subscription->fhp = handler;
			feed_parser_ctxt_cleanup (ctxt);
			(*(handler->feedParser)) (ctxt, xmlNode);
			success = TRUE;
			break;
		}

		// Text detection
		if (!xmlNode && handler->checkTextFormat && (*(handler->checkTextFormat))(ctxt->data, ctxt->subscription->source)) {
			ctxt->subscription->fhp = handler;
			feed_parser_ctxt_cleanup (ctxt);
			(*(handler->textFeedParser)) (ctxt, ctxt->data);
			success = TRUE;
			break;
		}

		handlerIter = handlerIter->next;
	}

	/* 4.) None of the feed formats did work, chance is high that we are
	       working on an HTML document. Let's look for feed links inside it! */
	if (!success) {
		ctxt->subscription->autoDiscoveryTries++;
		if (ctxt->subscription->autoDiscoveryTries > AUTO_DISCOVERY_MAX_REDIRECTS) {
			debug (DEBUG_UPDATE, "Stopping feed auto discovery (%s) after too many redirects (limit is %d)", subscription_get_source (ctxt->subscription), AUTO_DISCOVERY_MAX_REDIRECTS);
		} else {
			autoDiscovery = feed_parser_auto_discover (ctxt);
		}
	}

	/* 5.) try all HTML parsers (these are all HTML based content extractors), note how those MUST
	       be run after auto-discovery to not take precedence over not-yet discovered feed links */
	handlerIter = feed_parsers_get_list ();
	while (handlerIter) {
		feedHandlerPtr handler = (feedHandlerPtr)(handlerIter->data);

		if (htmlNode && handler && handler->checkFormat && handler->html && (*(handler->checkFormat))(htmlDoc, htmlNode)) {
			ctxt->subscription->fhp = handler;
			feed_parser_ctxt_cleanup (ctxt);
			(*(handler->feedParser)) (ctxt, htmlNode);
			success = TRUE;
			break;
		}
		handlerIter = handlerIter->next;
	}

	if (htmlDoc)
		xmlFreeDoc (htmlDoc);
	if (xmlDoc)
		xmlFreeDoc (xmlDoc);

	/* 6.) Update subscription error status */
	if (!success && !autoDiscovery) {
		/* Fuzzy test for HTML document */
		if ((strstr (ctxt->data, "<html>") || strstr (ctxt->data, "<HTML>") ||
		     strstr (ctxt->data, "<html ") || strstr (ctxt->data, "<HTML ")))
			ctxt->subscription->error = FETCH_ERROR_DISCOVER;
	} else {
		if (ctxt->subscription->fhp) {
			debug (DEBUG_UPDATE, "discovered feed format: %s", feed_type_fhp_to_str (ctxt->subscription->fhp));
			ctxt->subscription->autoDiscoveryTries = 0;
		} else {
			/* Auto discovery found a link that is being processed
			   asynchronously, for now we do not know wether it will
			   succeed. Still our auto-discovery was successful. */
		}
		success = TRUE;
		ctxt->subscription->error = FETCH_ERROR_NONE;
	}

	return success;
}
