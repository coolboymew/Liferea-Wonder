/**
 * @file page_scraper.c  Test cases for page feeds
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

#include <glib.h>
#include <string.h>

#include "debug.h"
#include "item.h"
#include "parsers/page_scraper.h"

/* a made-up shop result page: a menu block sharing the item class,
   items with an id attribute, a lazy loaded picture, a duplicate link */
static const gchar *shopPage =
	"<html><head><title>Search results</title></head><body>"
	"<div class=\"block\"><ul><li><a href=\"/cat/1\">Comics</a></li></ul></div>"
	"<div class=\"list\">"
	" <div class=\"block new\" data-itemidx=\"1\">"
	"  <div class=\"pic\"><a href=\"/item?id=1\"><img src=\"/img/1.jpg\"/></a></div>"
	"  <div class=\"title\"><p><a href=\"/item?id=1\">  First   item </a></p></div>"
	"  <div class=\"price\"><p>2,000 yen</p></div>"
	" </div>"
	" <div class=\"block\" data-itemidx=\"2\">"
	"  <div class=\"pic\"><a href=\"/item?id=2\"><img data-src=\"/img/2.jpg\" src=\"/img/blank.gif\"/></a></div>"
	"  <div class=\"title\"><p><a href=\"/item?id=2\">Second item</a></p></div>"
	"  <div class=\"price\"><p>3,500 yen</p></div>"
	" </div>"
	" <div class=\"block\" data-itemidx=\"3\">"
	"  <div class=\"pic\"><a href=\"https://other.example/item/3\"><img src=\"/img/3.jpg\"/></a></div>"
	"  <div class=\"title\"><p><a href=\"https://other.example/item/3\">Third item</a></p></div>"
	"  <div class=\"price\"><p>900 yen</p></div>"
	" </div>"
	" <div class=\"block\" data-itemidx=\"3\">"
	"  <div class=\"pic\"><a href=\"https://other.example/item/3\"><img src=\"/img/3.jpg\"/></a></div>"
	"  <div class=\"title\"><p><a href=\"https://other.example/item/3\">Third item again</a></p></div>"
	" </div>"
	"</div></body></html>";

static void
tc_css_to_xpath (void)
{
	const gchar *cases[][3] = {
		{ "div.block", "0", "//div[contains(concat(' ',normalize-space(@class),' '),' block ')]" },
		{ ".title a", "1", ".//*[contains(concat(' ',normalize-space(@class),' '),' title ')]//a" },
		{ "ul > li#x", "0", "//ul/li[@id='x']" },
		{ "div[data-itemidx]", "0", "//div[@data-itemidx]" },
		{ "a[href^=\"/item\"]", "0", "//a[starts-with(@href,'/item')]" },
		{ "//p[1]", "0", "//p[1]" },
		{ NULL, NULL, NULL }
	};

	for (guint i = 0; cases[i][0]; i++) {
		g_autofree gchar *xpath = page_scraper_css_to_xpath (cases[i][0], cases[i][1][0] == '1', NULL);
		g_assert_cmpstr (xpath, ==, cases[i][2]);
	}

	/* unsupported syntax is reported */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *xpath = page_scraper_css_to_xpath ("div:hover", FALSE, &error);
		g_assert_null (xpath);
		g_assert_nonnull (error);
	}
}

static void
tc_extract (void)
{
	pageScraperPtr		scraper = page_scraper_new ();
	g_autoptr(GError)	error = NULL;
	g_autofree gchar	*pageTitle = NULL;
	GList			*items;
	itemPtr			item;

	scraper->item = g_strdup ("div.block");
	scraper->title = g_strdup (".title");
	scraper->image = g_strdup ("img");
	scraper->text = g_strdup (".price");

	items = page_scraper_extract (scraper, shopPage, strlen (shopPage), "https://shop.example/search?q=x", &pageTitle, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (pageTitle, ==, "Search results");

	/* the menu block has no title, the duplicate link is dropped */
	g_assert_cmpuint (g_list_length (items), ==, 3);

	/* first seen dates keep the page order when sorting by date */
	g_assert_cmpint (((itemPtr)items->data)->time, >, ((itemPtr)items->next->data)->time);
	g_assert_false (((itemPtr)items->data)->validTime);

	item = (itemPtr)items->data;
	g_assert_cmpstr (item_get_title (item), ==, "First item");
	g_assert_cmpstr (item_get_source (item), ==, "https://shop.example/item?id=1");
	g_assert_cmpstr (item_get_id (item), ==, "https://shop.example/item?id=1");
	g_assert_true (item->validGuid);
	g_assert_nonnull (strstr (item_get_description (item), "https://shop.example/img/1.jpg"));
	g_assert_nonnull (strstr (item_get_description (item), "2,000 yen"));

	/* lazy loaded picture */
	item = (itemPtr)items->next->data;
	g_assert_nonnull (strstr (item_get_description (item), "https://shop.example/img/2.jpg"));

	item = (itemPtr)items->next->next->data;
	g_assert_cmpstr (item_get_source (item), ==, "https://other.example/item/3");

	g_list_free_full (items, g_object_unref);
	page_scraper_free (scraper);
}

static void
tc_detect (void)
{
	pageScraperPtr	scraper = page_scraper_detect (shopPage, strlen (shopPage));
	GList		*items;

	g_assert_nonnull (scraper);
	/* the id attribute keeps the menu block out */
	g_assert_cmpstr (scraper->item, ==, "div.block[data-itemidx]");
	g_assert_cmpstr (scraper->title, ==, ".title");
	g_assert_cmpstr (scraper->text, ==, ".price");

	items = page_scraper_extract (scraper, shopPage, strlen (shopPage), "https://shop.example/", NULL, NULL);
	g_assert_cmpuint (g_list_length (items), ==, 3);

	g_list_free_full (items, g_object_unref);
	page_scraper_free (scraper);
}

int
test_page_scraper (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

	if (g_strv_contains ((const gchar **)argv, "--debug"))
		debug_set_flags (DEBUG_PARSING);

	g_test_add_func ("/page_scraper/css_to_xpath", tc_css_to_xpath);
	g_test_add_func ("/page_scraper/extract", tc_extract);
	g_test_add_func ("/page_scraper/detect", tc_detect);

	return g_test_run ();
}
