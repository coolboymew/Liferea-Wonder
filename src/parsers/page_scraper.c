/**
 * @file page_scraper.c  feeds made from web pages without a feed
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

#include "parsers/page_scraper.h"

#include <libxml/xpath.h>
#include <string.h>

#include "common.h"
#include "debug.h"
#include "item.h"
#include "xml.h"

pageScraperPtr
page_scraper_new (void)
{
	return g_new0 (struct pageScraper, 1);
}

pageScraperPtr
page_scraper_copy (pageScraperPtr scraper)
{
	pageScraperPtr copy = page_scraper_new ();

	copy->item = g_strdup (scraper->item);
	copy->title = g_strdup (scraper->title);
	copy->link = g_strdup (scraper->link);
	copy->image = g_strdup (scraper->image);
	copy->text = g_strdup (scraper->text);

	return copy;
}

void
page_scraper_free (pageScraperPtr scraper)
{
	if (!scraper)
		return;

	g_free (scraper->item);
	g_free (scraper->title);
	g_free (scraper->link);
	g_free (scraper->image);
	g_free (scraper->text);
	g_free (scraper);
}

/* CSS selector subset to XPath */

static gchar *
page_scraper_xpath_literal (const gchar *value)
{
	/* XPath 1.0 has no escaping, choose the quote not used */
	if (strchr (value, '\''))
		return g_strdup_printf ("\"%s\"", value);
	return g_strdup_printf ("'%s'", value);
}

static gboolean
page_scraper_is_name_char (gchar c)
{
	return g_ascii_isalnum (c) || c == '-' || c == '_';
}

gchar *
page_scraper_css_to_xpath (const gchar *selector, gboolean relative, GError **error)
{
	GString		*xpath;
	const gchar	*p;
	gboolean	child = FALSE;

	if (!selector)
		return NULL;
	while (g_ascii_isspace (*selector))
		selector++;
	if (!*selector)
		return NULL;

	/* XPath given as is */
	if (selector[0] == '/' || selector[0] == '(' || g_str_has_prefix (selector, "./"))
		return g_strdup (selector);

	xpath = g_string_new (relative?".":"");
	p = selector;

	while (*p) {
		/* combinators */
		gboolean sawSpace = FALSE;
		while (g_ascii_isspace (*p)) {
			p++;
			sawSpace = TRUE;
		}
		if (*p == '>') {
			child = TRUE;
			p++;
			continue;
		}
		if (!*p)
			break;
		(void)sawSpace;

		g_string_append (xpath, child?"/":"//");
		child = FALSE;

		/* element name */
		if (*p == '*') {
			g_string_append_c (xpath, '*');
			p++;
		} else if (page_scraper_is_name_char (*p)) {
			const gchar *start = p;
			while (page_scraper_is_name_char (*p))
				p++;
			g_string_append_len (xpath, start, p - start);
		} else {
			g_string_append_c (xpath, '*');
		}

		/* classes, ids and attributes of this element */
		while (*p == '.' || *p == '#' || *p == '[') {
			if (*p == '.' || *p == '#') {
				gchar		kind = *p++;
				const gchar	*start = p;

				while (page_scraper_is_name_char (*p))
					p++;
				if (p == start)
					goto unsupported;
				if (kind == '.')
					g_string_append_printf (xpath, "[contains(concat(' ',normalize-space(@class),' '),' %.*s ')]", (gint)(p - start), start);
				else
					g_string_append_printf (xpath, "[@id='%.*s']", (gint)(p - start), start);
			} else {
				const gchar		*start = ++p;
				g_autofree gchar	*attr = NULL, *value = NULL, *literal = NULL;
				gchar			op = 0;

				while (page_scraper_is_name_char (*p) || *p == ':')
					p++;
				attr = g_strndup (start, p - start);
				if (!*attr)
					goto unsupported;

				if (*p == '*' || *p == '^' || *p == '$' || *p == '~')
					op = *p++;
				if (*p == '=') {
					gchar quote = 0;

					if (!op)
						op = '=';
					p++;
					if (*p == '"' || *p == '\'')
						quote = *p++;
					start = p;
					while (*p && (quote?(*p != quote):(*p != ']')))
						p++;
					value = g_strndup (start, p - start);
					if (quote && *p)
						p++;
				}
				if (*p != ']')
					goto unsupported;
				p++;

				if (!value) {
					g_string_append_printf (xpath, "[@%s]", attr);
					continue;
				}
				literal = page_scraper_xpath_literal (value);
				switch (op) {
					case '*': g_string_append_printf (xpath, "[contains(@%s,%s)]", attr, literal); break;
					case '^': g_string_append_printf (xpath, "[starts-with(@%s,%s)]", attr, literal); break;
					case '$': g_string_append_printf (xpath, "[substring(@%s,string-length(@%s)-string-length(%s)+1)=%s]", attr, attr, literal, literal); break;
					case '~': g_string_append_printf (xpath, "[contains(concat(' ',normalize-space(@%s),' '),concat(' ',%s,' '))]", attr, literal); break;
					default:  g_string_append_printf (xpath, "[@%s=%s]", attr, literal); break;
				}
			}
		}

		if (*p && !g_ascii_isspace (*p) && *p != '>')
			goto unsupported;
	}

	return g_string_free (xpath, FALSE);

unsupported:
	g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
	             _("The selector \"%s\" is not supported near \"%s\". Use tag, .class, #id, [attribute] or [attribute=value], separated by spaces or >."),
	             selector, p);
	g_string_free (xpath, TRUE);
	return NULL;
}

/* extraction */

static GSList *
page_scraper_find (xmlXPathContextPtr xctx, xmlNodePtr context, const gchar *xpath)
{
	xmlXPathObjectPtr	result;
	GSList			*nodes = NULL;

	xctx->node = context;
	result = xmlXPathEvalExpression (BAD_CAST xpath, xctx);
	if (result && result->nodesetval)
		for (gint i = 0; i < result->nodesetval->nodeNr; i++)
			nodes = g_slist_append (nodes, result->nodesetval->nodeTab[i]);
	xmlXPathFreeObject (result);

	return nodes;
}

/* The text of an element with whitespace collapsed (or NULL if empty) */
static gchar *
page_scraper_text (xmlNodePtr node)
{
	g_autofree gchar	*raw = (gchar *)xmlNodeGetContent (node);
	g_auto(GStrv)		words = NULL;
	GString			*text = g_string_new (NULL);

	if (raw) {
		words = g_strsplit_set (raw, " \t\r\n\xc2\xa0", -1);
		for (guint i = 0; words[i]; i++) {
			if (!*words[i])
				continue;
			if (text->len)
				g_string_append_c (text, ' ');
			g_string_append (text, words[i]);
		}
	}

	if (!text->len) {
		g_string_free (text, TRUE);
		return NULL;
	}
	return g_string_free (text, FALSE);
}

/* An attribute of the element, or of its first descendant having it */
static gchar *
page_scraper_attribute (xmlXPathContextPtr xctx, xmlNodePtr node, const gchar *attr)
{
	g_autofree gchar	*xpath = NULL;
	GSList			*found;
	gchar			*value = NULL;

	value = (gchar *)xmlGetProp (node, BAD_CAST attr);
	if (value && *value)
		return value;
	xmlFree (value);

	xpath = g_strdup_printf (".//*[@%s]", attr);
	found = page_scraper_find (xctx, node, xpath);
	if (found)
		value = (gchar *)xmlGetProp ((xmlNodePtr)found->data, BAD_CAST attr);
	g_slist_free (found);

	return value;
}

static gchar *
page_scraper_absolute (const gchar *url, const gchar *baseUrl)
{
	if (!url || !*url || g_str_has_prefix (url, "javascript:"))
		return NULL;

	return (gchar *)common_build_url (url, baseUrl);
}

/* Evaluates a selector below an item: the first match or NULL */
static xmlNodePtr
page_scraper_first (xmlXPathContextPtr xctx, xmlNodePtr item, const gchar *selector, GError **error)
{
	g_autofree gchar	*xpath = NULL;
	GSList			*found;
	xmlNodePtr		node = NULL;

	if (!selector || !*selector)
		return NULL;

	xpath = page_scraper_css_to_xpath (selector, TRUE, error);
	if (!xpath)
		return NULL;

	found = page_scraper_find (xctx, item, xpath);
	if (found)
		node = (xmlNodePtr)found->data;
	g_slist_free (found);

	return node;
}

GList *
page_scraper_extract (pageScraperPtr scraper, const gchar *data, gsize size, const gchar *baseUrl, gchar **pageTitle, GError **error)
{
	g_autofree gchar	*itemXPath = NULL;
	xmlDocPtr		doc;
	xmlXPathContextPtr	xctx;
	GSList			*itemNodes, *iter;
	GList			*items = NULL;
	GHashTable		*seen;
	gint64			now = g_get_real_time () / G_USEC_PER_SEC;
	guint			position = 0;

	if (!scraper || !scraper->item || !*scraper->item) {
		g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE, _("No item selector given."));
		return NULL;
	}

	itemXPath = page_scraper_css_to_xpath (scraper->item, FALSE, error);
	if (!itemXPath)
		return NULL;

	doc = xhtml_parse (data, (gint)size);
	if (!doc) {
		g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE, _("The page could not be read as HTML."));
		return NULL;
	}

	xctx = xmlXPathNewContext (doc);
	if (pageTitle) {
		GSList *titles = page_scraper_find (xctx, xmlDocGetRootElement (doc), "//title");
		*pageTitle = titles?page_scraper_text ((xmlNodePtr)titles->data):NULL;
		g_slist_free (titles);
	}

	seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	itemNodes = page_scraper_find (xctx, xmlDocGetRootElement (doc), itemXPath);
	for (iter = itemNodes; iter; iter = g_slist_next (iter)) {
		xmlNodePtr		node = (xmlNodePtr)iter->data, match;
		g_autofree gchar	*title = NULL, *link = NULL, *image = NULL, *text = NULL, *href = NULL, *src = NULL;
		GString			*description;
		itemPtr			item;

		/* title (items without one are not items, e.g. menu blocks) */
		match = scraper->title?page_scraper_first (xctx, node, scraper->title, error):node;
		if (match)
			title = page_scraper_text (match);
		if (!title)
			continue;

		/* link: the given element, or the first link in the title or item */
		if (scraper->link && *scraper->link) {
			xmlNodePtr linkNode = page_scraper_first (xctx, node, scraper->link, error);
			if (linkNode)
				href = page_scraper_attribute (xctx, linkNode, "href");
		}
		if (!href && match)
			href = page_scraper_attribute (xctx, match, "href");
		if (!href)
			href = page_scraper_attribute (xctx, node, "href");
		link = page_scraper_absolute (href, baseUrl);

		/* picture (lazy loading pages keep it in data-src) */
		if (scraper->image && *scraper->image) {
			xmlNodePtr imageNode = page_scraper_first (xctx, node, scraper->image, error);
			if (imageNode) {
				src = page_scraper_attribute (xctx, imageNode, "data-src");
				if (!src)
					src = page_scraper_attribute (xctx, imageNode, "src");
			}
			image = page_scraper_absolute (src, baseUrl);
		}

		if (scraper->text && *scraper->text) {
			xmlNodePtr textNode = page_scraper_first (xctx, node, scraper->text, error);
			if (textNode)
				text = page_scraper_text (textNode);
		}

		/* the same item may be listed twice (e.g. picture and title links) */
		if (link) {
			if (g_hash_table_contains (seen, link))
				continue;
			g_hash_table_add (seen, g_strdup (link));
		}

		item = item_new ();
		item_set_title (item, title);

		/* Pages rarely give dates: an item is dated when first seen, a
		   second apart in page order so the top of the page (usually the
		   newest) sorts first. Not marked as valid time, so updates of a
		   known item (e.g. a new price) keep its first seen date. */
		item->time = now - position++;
		if (link) {
			item_set_source (item, link);
			item_set_id (item, link);
			item->validGuid = TRUE;
		} else {
			g_autofree gchar *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, title, -1);
			item_set_id (item, hash);
		}

		description = g_string_new (NULL);
		if (image) {
			g_autofree gchar *escaped = g_markup_escape_text (image, -1);
			g_string_append_printf (description, "<p><img src=\"%s\"/></p>", escaped);
		}
		if (text) {
			g_autofree gchar *escaped = g_markup_escape_text (text, -1);
			g_string_append_printf (description, "<p>%s</p>", escaped);
		}
		if (description->len)
			item_set_description (item, description->str);
		g_string_free (description, TRUE);

		items = g_list_append (items, item);
	}
	g_slist_free (itemNodes);
	g_hash_table_destroy (seen);

	xmlXPathFreeContext (xctx);
	xmlFreeDoc (doc);

	return items;
}

/* selector detection */

typedef struct {
	guint		count;		/* elements with a link and a picture */
	guint		size;		/* their descendant elements (bigger = more complete item) */
	xmlNodePtr	first;
	GSList		*nodes;
} candidate;

static guint
page_scraper_count_elements (xmlNodePtr node)
{
	guint count = 0;

	for (xmlNodePtr child = node->children; child; child = child->next)
		if (child->type == XML_ELEMENT_NODE)
			count += 1 + page_scraper_count_elements (child);

	return count;
}

static gboolean
page_scraper_has_descendant (xmlNodePtr node, const gchar *name, const gchar *attr)
{
	for (xmlNodePtr child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (!xmlStrcasecmp (child->name, BAD_CAST name)) {
			xmlChar *value = attr?xmlGetProp (child, BAD_CAST attr):NULL;
			gboolean ok = !attr || (value && *value);
			xmlFree (value);
			if (ok)
				return TRUE;
		}
		if (page_scraper_has_descendant (child, name, attr))
			return TRUE;
	}

	return FALSE;
}

static void
page_scraper_collect (xmlNodePtr node, GHashTable *candidates)
{
	for (xmlNodePtr child = node->children; child; child = child->next) {
		g_autofree gchar	*classes = NULL;
		g_auto(GStrv)		tokens = NULL;

		if (child->type != XML_ELEMENT_NODE)
			continue;

		classes = (gchar *)xmlGetProp (child, BAD_CAST "class");
		if (classes &&
		    page_scraper_has_descendant (child, "a", "href") &&
		    page_scraper_has_descendant (child, "img", NULL)) {
			tokens = g_strsplit_set (classes, " \t\r\n", -1);
			for (guint i = 0; tokens[i]; i++) {
				g_autofree gchar	*key = NULL;
				candidate		*c;

				if (!*tokens[i])
					continue;
				key = g_strdup_printf ("%s.%s", (const gchar *)child->name, tokens[i]);
				c = g_hash_table_lookup (candidates, key);
				if (!c) {
					c = g_new0 (candidate, 1);
					c->first = child;
					g_hash_table_insert (candidates, g_steal_pointer (&key), c);
				}
				c->count++;
				c->size += MIN (page_scraper_count_elements (child), 50u);
				c->nodes = g_slist_append (c->nodes, child);
			}
		}

		page_scraper_collect (child, candidates);
	}
}

static void
page_scraper_candidate_free (gpointer data)
{
	candidate *c = (candidate *)data;

	g_slist_free (c->nodes);
	g_free (c);
}

/* A class of a descendant of the first item containing one of the words */
static gchar *
page_scraper_find_class (xmlNodePtr node, const gchar **words)
{
	for (xmlNodePtr child = node->children; child; child = child->next) {
		g_autofree gchar	*classes = NULL;
		gchar			*found;

		if (child->type != XML_ELEMENT_NODE)
			continue;

		classes = (gchar *)xmlGetProp (child, BAD_CAST "class");
		if (classes) {
			g_auto(GStrv) tokens = g_strsplit_set (classes, " \t\r\n", -1);
			for (guint i = 0; tokens[i]; i++)
				for (guint w = 0; words[w]; w++)
					if (*tokens[i] && strstr (tokens[i], words[w]))
						return g_strdup_printf (".%s", tokens[i]);
		}

		if ((found = page_scraper_find_class (child, words)))
			return found;
	}

	return NULL;
}

pageScraperPtr
page_scraper_detect (const gchar *data, gsize size)
{
	xmlDocPtr		doc = xhtml_parse (data, (gint)size);
	GHashTable		*candidates;
	GHashTableIter		iter;
	gpointer		key, value;
	const gchar		*bestKey = NULL;
	candidate		*best = NULL;
	pageScraperPtr		scraper = NULL;
	const gchar		*titleWords[] = { "title", "name", "heading", NULL };
	const gchar		*textWords[] = { "price", "cost", "date", "summary", "desc", NULL };

	if (!doc)
		return NULL;

	candidates = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, page_scraper_candidate_free);
	page_scraper_collect ((xmlNodePtr)doc, candidates);

	/* the most repeated block wins, among equals the most complete
	   (an outer item block rather than its picture box) */
	g_hash_table_iter_init (&iter, candidates);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		candidate *c = (candidate *)value;

		if (c->count < 3)
			continue;
		if (!best || c->count > best->count ||
		    (c->count == best->count && c->size > best->size)) {
			best = c;
			bestKey = (const gchar *)key;
		}
	}

	if (best) {
		GString *item = g_string_new (bestKey);

		/* an attribute all found blocks have (e.g. an item id) keeps
		   other blocks with the same class (menus...) out, id-like
		   names first (data-itemid...) */
		const gchar *chosen = NULL;
		for (guint pass = 0; pass < 2 && !chosen; pass++) {
			for (xmlAttrPtr attr = best->first->properties; attr; attr = attr->next) {
				const gchar	*name = (const gchar *)attr->name;
				gboolean	all = TRUE;

				if (!strcmp (name, "class") || !strcmp (name, "style") || !strcmp (name, "id"))
					continue;
				if (0 == pass && !strstr (name, "id"))
					continue;
				for (GSList *n = best->nodes; n && all; n = g_slist_next (n))
					all = (NULL != xmlHasProp ((xmlNodePtr)n->data, attr->name));
				if (all) {
					chosen = name;
					break;
				}
			}
		}
		if (chosen)
			g_string_append_printf (item, "[%s]", chosen);

		scraper = page_scraper_new ();
		scraper->item = g_string_free (item, FALSE);
		scraper->title = page_scraper_find_class (best->first, titleWords);
		if (!scraper->title)
			scraper->title = g_strdup ("a");
		scraper->image = g_strdup ("img");
		scraper->text = page_scraper_find_class (best->first, textWords);
		debug (DEBUG_PARSING, "page scraper detected item=%s title=%s text=%s", scraper->item, scraper->title, scraper->text?scraper->text:"");
	}

	g_hash_table_destroy (candidates);
	xmlFreeDoc (doc);

	return scraper;
}

/* feed parsing */

static feedHandlerPtr pageScraperHandler = NULL;

gboolean
page_scraper_parse (feedParserCtxtPtr ctxt)
{
	g_autoptr(GError)	error = NULL;
	gchar			*pageTitle = NULL;
	GList			*items;

	items = page_scraper_extract (ctxt->subscription->scraper, ctxt->data, ctxt->dataLength,
	                              subscription_get_source (ctxt->subscription), &pageTitle, &error);

	if (error) {
		g_string_append (ctxt->subscription->parseErrors, error->message);
		ctxt->subscription->error = FETCH_ERROR_XML;
	} else if (!items) {
		g_string_append (ctxt->subscription->parseErrors, _("No items found on the page with the given item and title selectors."));
		ctxt->subscription->error = FETCH_ERROR_XML;
	}

	if (!items) {
		g_free (pageTitle);
		g_list_free_full (items, g_object_unref);
		return FALSE;
	}

	ctxt->subscription->fhp = pageScraperHandler;
	ctxt->subscription->error = FETCH_ERROR_NONE;
	g_free (ctxt->title);
	ctxt->title = pageTitle;
	ctxt->items = items;
	debug (DEBUG_PARSING, "page scraper found %u items on %s", g_list_length (items), subscription_get_source (ctxt->subscription));

	return TRUE;
}

feedHandlerPtr
page_scraper_init_feed_handler (void)
{
	/* no format checks: only used for subscriptions set up as page feeds */
	pageScraperHandler = g_new0 (struct feedHandler, 1);
	pageScraperHandler->typeStr = PAGE_SCRAPER_TYPE;
	pageScraperHandler->html = TRUE;

	return pageScraperHandler;
}
