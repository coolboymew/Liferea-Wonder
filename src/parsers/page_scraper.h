/*
 * @file page_scraper.h  feeds made from web pages without a feed
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

#ifndef _PAGE_SCRAPER_H
#define _PAGE_SCRAPER_H

#include "feed_parser.h"

/* A "page feed" turns a web page listing things (shop search results,
   news lists...) into feed items. The user describes one item with CSS
   selectors (a simple subset, or XPath when starting with '/' or '.'):

     item   matches each item block, e.g. "div.block[data-itemidx]"
     title  inside an item, its text is the item title, e.g. ".title a"
     link   inside an item, its href (default: first link in the title or item)
     image  inside an item, its src (optional)
     text   inside an item, text shown below the picture, e.g. ".date"
     price  inside an item, its price (optional): shown in the item list,
            a changed price updates the entry (see itemset.c)

   Each item's link is its GUID, so only items not seen before are new. */

typedef struct pageScraper {
	gchar	*item;
	gchar	*title;
	gchar	*link;
	gchar	*image;
	gchar	*text;
	gchar	*price;
} *pageScraperPtr;

#define PAGE_SCRAPER_TYPE	"pagescraper"

pageScraperPtr page_scraper_new (void);
pageScraperPtr page_scraper_copy (pageScraperPtr scraper);
void page_scraper_free (pageScraperPtr scraper);

/**
 * page_scraper_css_to_xpath: (skip)
 * @selector:	CSS selector (subset) or XPath
 * @relative:	TRUE to match below a context node, FALSE in the whole page
 * @error:	(out) (nullable): unsupported selector syntax
 *
 * Returns: (transfer full): the XPath expression (or NULL)
 */
gchar * page_scraper_css_to_xpath (const gchar *selector, gboolean relative, GError **error);

/**
 * page_scraper_extract: (skip)
 * @scraper:	the selectors
 * @data:	the downloaded page
 * @size:	its size
 * @baseUrl:	the page address for resolving relative links
 * @pageTitle:	(out) (nullable): the page title
 * @error:	(out) (nullable): selector errors
 *
 * Returns: (transfer full): list of items (itemPtr), newest (first on the page) first
 */
GList * page_scraper_extract (pageScraperPtr scraper, const gchar *data, gsize size, const gchar *baseUrl, gchar **pageTitle, GError **error);

/**
 * page_scraper_detect: (skip)
 * @data:	the downloaded page
 * @size:	its size
 *
 * Guesses selectors for pages listing many similar blocks with a link
 * and a picture (shop and search result pages).
 *
 * Returns: (transfer full) (nullable): suggested selectors
 */
pageScraperPtr page_scraper_detect (const gchar *data, gsize size);

/**
 * page_scraper_parse: (skip)
 * @ctxt:	parsing context of a subscription with a scraper
 *
 * Feed parsing for page feeds (called by feed_parse()).
 *
 * Returns: TRUE if items were found
 */
gboolean page_scraper_parse (feedParserCtxtPtr ctxt);

/**
 * page_scraper_init_feed_handler: (skip)
 *
 * Returns: the feed handler registering the "pagescraper" type
 */
feedHandlerPtr page_scraper_init_feed_handler (void);

#endif
