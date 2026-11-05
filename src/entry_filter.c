/**
 * @file entry_filter.c  dropping or tagging entries on feed update
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

#include "entry_filter.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "common.h"
#include "db.h"
#include "debug.h"
#include "item.h"
#include "metadata.h"

#define ENTRY_FILTER_FILE	"entry_filters.json"

/* Rejected entries are remembered while they are still in their feed
   (each update refreshes their last_seen time). Entries not seen for
   this many days have left the feed and are forgotten, checked once a
   day while running. */
#define ENTRY_FILTER_KEEP_DAYS		30
#define ENTRY_FILTER_PRUNE_INTERVAL	(G_GINT64_CONSTANT (24 * 60 * 60) * G_USEC_PER_SEC)

static GSList	*filters = NULL;
static gboolean	loaded = FALSE;

entryFilterPtr
entry_filter_new (void)
{
	entryFilterPtr filter = g_new0 (struct entryFilter, 1);

	filter->id = g_uuid_string_random ();
	filter->name = g_strdup ("");
	filter->pattern = g_strdup ("");
	filter->enabled = TRUE;
	filter->match = ENTRY_FILTER_MATCH_WORD;
	filter->fields = ENTRY_FILTER_FIELD_TITLE | ENTRY_FILTER_FIELD_CONTENT;
	filter->action = ENTRY_FILTER_ACTION_EXCLUDE;

	return filter;
}

entryFilterPtr
entry_filter_copy (entryFilterPtr filter)
{
	entryFilterPtr copy = g_new0 (struct entryFilter, 1);

	copy->id = g_strdup (filter->id);
	copy->name = g_strdup (filter->name);
	copy->pattern = g_strdup (filter->pattern);
	copy->enabled = filter->enabled;
	copy->match = filter->match;
	copy->caseSensitive = filter->caseSensitive;
	copy->fields = filter->fields;
	copy->action = filter->action;
	copy->allFeeds = filter->allFeeds;
	copy->nodeIds = g_slist_copy_deep (filter->nodeIds, (GCopyFunc)g_strdup, NULL);
	entry_filter_compile (copy, NULL);

	return copy;
}

void
entry_filter_free (entryFilterPtr filter)
{
	if (!filter)
		return;

	g_free (filter->id);
	g_free (filter->name);
	g_free (filter->pattern);
	g_slist_free_full (filter->nodeIds, g_free);
	g_clear_pointer (&filter->regex, g_regex_unref);
	g_free (filter);
}

gboolean
entry_filter_compile (entryFilterPtr filter, GError **error)
{
	g_autofree gchar	*escaped = NULL, *pattern = NULL;
	GRegexCompileFlags	flags = G_REGEX_OPTIMIZE;

	g_clear_pointer (&filter->regex, g_regex_unref);

	if (!filter->pattern || !*filter->pattern) {
		g_set_error_literal (error, G_REGEX_ERROR, G_REGEX_ERROR_COMPILE, _("The text to match is empty."));
		return FALSE;
	}

	if (!filter->caseSensitive)
		flags |= G_REGEX_CASELESS;

	switch (filter->match) {
		case ENTRY_FILTER_MATCH_WORD:
			/* (*UCP) makes \b work for non-ASCII letters too */
			escaped = g_regex_escape_string (filter->pattern, -1);
			pattern = g_strdup_printf ("(*UCP)\\b%s\\b", escaped);
			break;
		case ENTRY_FILTER_MATCH_PHRASE:
			pattern = g_regex_escape_string (filter->pattern, -1);
			break;
		case ENTRY_FILTER_MATCH_REGEX:
		default:
			pattern = g_strdup (filter->pattern);
			break;
	}

	filter->regex = g_regex_new (pattern, flags, 0, error);
	return (NULL != filter->regex);
}

/* persistence */

static const gchar *
entry_filter_match_to_string (entryFilterMatch match)
{
	switch (match) {
		case ENTRY_FILTER_MATCH_PHRASE:	return "phrase";
		case ENTRY_FILTER_MATCH_REGEX:	return "regex";
		default:			return "word";
	}
}

static entryFilterMatch
entry_filter_match_from_string (const gchar *str)
{
	if (!g_strcmp0 (str, "phrase"))
		return ENTRY_FILTER_MATCH_PHRASE;
	if (!g_strcmp0 (str, "regex"))
		return ENTRY_FILTER_MATCH_REGEX;
	return ENTRY_FILTER_MATCH_WORD;
}

static const gchar *
entry_filter_action_to_string (entryFilterAction action)
{
	switch (action) {
		case ENTRY_FILTER_ACTION_INCLUDE_ONLY:	return "include-only";
		case ENTRY_FILTER_ACTION_MARK_READ:	return "mark-read";
		case ENTRY_FILTER_ACTION_FLAG:		return "flag";
		default:				return "exclude";
	}
}

static entryFilterAction
entry_filter_action_from_string (const gchar *str)
{
	if (!g_strcmp0 (str, "include-only"))
		return ENTRY_FILTER_ACTION_INCLUDE_ONLY;
	if (!g_strcmp0 (str, "mark-read"))
		return ENTRY_FILTER_ACTION_MARK_READ;
	if (!g_strcmp0 (str, "flag"))
		return ENTRY_FILTER_ACTION_FLAG;
	return ENTRY_FILTER_ACTION_EXCLUDE;
}

static const struct {
	entryFilterField	field;
	const gchar		*name;
} fieldNames[] = {
	{ ENTRY_FILTER_FIELD_TITLE,	"title" },
	{ ENTRY_FILTER_FIELD_CONTENT,	"content" },
	{ ENTRY_FILTER_FIELD_AUTHOR,	"author" },
	{ ENTRY_FILTER_FIELD_CATEGORY,	"category" },
	{ ENTRY_FILTER_FIELD_LINK,	"link" }
};

static void
entry_filter_load (void)
{
	g_autofree gchar	*filename = common_create_config_filename (ENTRY_FILTER_FILE);
	g_autoptr(JsonParser)	parser = json_parser_new ();
	g_autoptr(GError)	error = NULL;
	JsonNode		*root;
	JsonArray		*array;

	loaded = TRUE;

	if (!g_file_test (filename, G_FILE_TEST_EXISTS))
		return;

	if (!json_parser_load_from_file (parser, filename, &error)) {
		g_warning ("Could not load entry filters from %s: %s", filename, error->message);
		return;
	}

	root = json_parser_get_root (parser);
	if (!JSON_NODE_HOLDS_OBJECT (root) ||
	    !json_object_has_member (json_node_get_object (root), "filters"))
		return;

	array = json_object_get_array_member (json_node_get_object (root), "filters");
	for (guint i = 0; i < json_array_get_length (array); i++) {
		JsonObject	*obj = json_array_get_object_element (array, i);
		entryFilterPtr	filter;

		if (!obj)
			continue;

		filter = entry_filter_new ();
		if (json_object_has_member (obj, "id")) {
			g_free (filter->id);
			filter->id = g_strdup (json_object_get_string_member (obj, "id"));
		}
		g_free (filter->name);
		filter->name = g_strdup (json_object_get_string_member_with_default (obj, "name", ""));
		g_free (filter->pattern);
		filter->pattern = g_strdup (json_object_get_string_member_with_default (obj, "pattern", ""));
		filter->enabled = json_object_get_boolean_member_with_default (obj, "enabled", TRUE);
		filter->match = entry_filter_match_from_string (json_object_get_string_member_with_default (obj, "match", "word"));
		filter->caseSensitive = json_object_get_boolean_member_with_default (obj, "caseSensitive", FALSE);
		filter->action = entry_filter_action_from_string (json_object_get_string_member_with_default (obj, "action", "exclude"));
		filter->allFeeds = json_object_get_boolean_member_with_default (obj, "allFeeds", FALSE);

		if (json_object_has_member (obj, "fields")) {
			JsonArray *fields = json_object_get_array_member (obj, "fields");

			filter->fields = 0;
			for (guint j = 0; j < json_array_get_length (fields); j++) {
				const gchar *name = json_array_get_string_element (fields, j);
				for (guint k = 0; k < G_N_ELEMENTS (fieldNames); k++)
					if (!g_strcmp0 (name, fieldNames[k].name))
						filter->fields |= fieldNames[k].field;
			}
		}

		if (json_object_has_member (obj, "nodes")) {
			JsonArray *nodes = json_object_get_array_member (obj, "nodes");

			for (guint j = 0; j < json_array_get_length (nodes); j++)
				filter->nodeIds = g_slist_append (filter->nodeIds, g_strdup (json_array_get_string_element (nodes, j)));
		}

		if (!entry_filter_compile (filter, &error)) {
			g_warning ("Entry filter \"%s\" has an invalid pattern and is ignored: %s", filter->name, error?error->message:"");
			g_clear_error (&error);
		}

		filters = g_slist_append (filters, filter);
	}

	debug (DEBUG_UPDATE, "loaded %u entry filters", g_slist_length (filters));
}

static void
entry_filter_save (void)
{
	g_autofree gchar	*filename = common_create_config_filename (ENTRY_FILTER_FILE);
	g_autoptr(JsonBuilder)	b = json_builder_new ();
	g_autoptr(JsonGenerator) gen = json_generator_new ();
	g_autoptr(JsonNode)	root = NULL;
	g_autoptr(GError)	error = NULL;

	json_builder_begin_object (b);
	json_builder_set_member_name (b, "filters");
	json_builder_begin_array (b);
	for (GSList *iter = filters; iter; iter = g_slist_next (iter)) {
		entryFilterPtr filter = (entryFilterPtr)iter->data;

		json_builder_begin_object (b);
		json_builder_set_member_name (b, "id");
		json_builder_add_string_value (b, filter->id);
		json_builder_set_member_name (b, "name");
		json_builder_add_string_value (b, filter->name);
		json_builder_set_member_name (b, "enabled");
		json_builder_add_boolean_value (b, filter->enabled);
		json_builder_set_member_name (b, "pattern");
		json_builder_add_string_value (b, filter->pattern);
		json_builder_set_member_name (b, "match");
		json_builder_add_string_value (b, entry_filter_match_to_string (filter->match));
		json_builder_set_member_name (b, "caseSensitive");
		json_builder_add_boolean_value (b, filter->caseSensitive);
		json_builder_set_member_name (b, "action");
		json_builder_add_string_value (b, entry_filter_action_to_string (filter->action));

		json_builder_set_member_name (b, "fields");
		json_builder_begin_array (b);
		for (guint k = 0; k < G_N_ELEMENTS (fieldNames); k++)
			if (filter->fields & fieldNames[k].field)
				json_builder_add_string_value (b, fieldNames[k].name);
		json_builder_end_array (b);

		json_builder_set_member_name (b, "allFeeds");
		json_builder_add_boolean_value (b, filter->allFeeds);
		json_builder_set_member_name (b, "nodes");
		json_builder_begin_array (b);
		for (GSList *n = filter->nodeIds; n; n = g_slist_next (n))
			json_builder_add_string_value (b, (gchar *)n->data);
		json_builder_end_array (b);
		json_builder_end_object (b);
	}
	json_builder_end_array (b);
	json_builder_end_object (b);

	root = json_builder_get_root (b);
	json_generator_set_root (gen, root);
	json_generator_set_pretty (gen, TRUE);
	if (!json_generator_to_file (gen, filename, &error))
		g_warning ("Could not save entry filters to %s: %s", filename, error->message);
}

GSList *
entry_filter_get_all (void)
{
	if (!loaded)
		entry_filter_load ();

	return filters;
}

void
entry_filter_set_all (GSList *newFilters)
{
	g_slist_free_full (filters, (GDestroyNotify)entry_filter_free);
	filters = newFilters;
	loaded = TRUE;
	entry_filter_save ();

	/* Previously rejected entries might pass the new rules */
	db_filtered_items_clear ();
}

/* matching */

gboolean
entry_filter_applies_to (entryFilterPtr filter, Node *node)
{
	if (filter->allFeeds)
		return TRUE;

	for (; node; node = node->parent)
		if (g_slist_find_custom (filter->nodeIds, node->id, (GCompareFunc)g_strcmp0))
			return TRUE;

	return FALSE;
}

static gboolean
entry_filter_match_text (entryFilterPtr filter, const gchar *text)
{
	return text && g_regex_match (filter->regex, text, 0, NULL);
}

/* Content is HTML, we only want to match the readable text */
static gchar *
entry_filter_strip_markup (const gchar *html)
{
	static GRegex	*tags = NULL;
	gchar		*text;

	if (!html)
		return NULL;

	if (!tags)
		tags = g_regex_new ("<[^>]*>", G_REGEX_OPTIMIZE, 0, NULL);

	text = g_regex_replace_literal (tags, html, -1, 0, " ", 0, NULL);
	text = common_strreplace (text, "&nbsp;", " ");
	text = common_strreplace (text, "&amp;", "&");
	text = common_strreplace (text, "&lt;", "<");
	text = common_strreplace (text, "&gt;", ">");
	text = common_strreplace (text, "&quot;", "\"");
	text = common_strreplace (text, "&#39;", "'");

	return text;
}

/* Metadata values (author, category) may be HTML escaped too */
static gboolean
entry_filter_match_list (entryFilterPtr filter, GSList *values)
{
	for (GSList *iter = values; iter; iter = g_slist_next (iter)) {
		g_autofree gchar *text = entry_filter_strip_markup ((const gchar *)iter->data);
		if (entry_filter_match_text (filter, text))
			return TRUE;
	}

	return FALSE;
}

static gboolean
entry_filter_matches (entryFilterPtr filter, itemPtr item, gchar **content)
{
	if (!filter->regex)
		return FALSE;

	if ((filter->fields & ENTRY_FILTER_FIELD_TITLE) &&
	    entry_filter_match_text (filter, item->title))
		return TRUE;

	if (filter->fields & ENTRY_FILTER_FIELD_CONTENT) {
		/* strip the markup only once for all filters */
		if (!*content)
			*content = entry_filter_strip_markup (item->description);
		if (entry_filter_match_text (filter, *content))
			return TRUE;
	}

	if ((filter->fields & ENTRY_FILTER_FIELD_AUTHOR) &&
	    (entry_filter_match_list (filter, metadata_list_get_values (item->metadata, "author")) ||
	     entry_filter_match_list (filter, metadata_list_get_values (item->metadata, "creator"))))
		return TRUE;

	if ((filter->fields & ENTRY_FILTER_FIELD_CATEGORY) &&
	    entry_filter_match_list (filter, metadata_list_get_values (item->metadata, "category")))
		return TRUE;

	if ((filter->fields & ENTRY_FILTER_FIELD_LINK) &&
	    entry_filter_match_text (filter, item->source))
		return TRUE;

	return FALSE;
}

/* The key identifying an entry across updates: its GUID if it has
   one, otherwise its link, otherwise a hash of its title. */
static gchar *
entry_filter_get_item_key (itemPtr item)
{
	if (item->sourceId && *item->sourceId)
		return g_strdup_printf ("id:%s", item->sourceId);
	if (item->source && *item->source)
		return g_strdup_printf ("link:%s", item->source);

	g_autofree gchar *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, item->title?item->title:"", -1);
	return g_strdup_printf ("title:%s", hash);
}

GList *
entry_filter_apply (Node *node, GList *items)
{
	GSList		*applicable = NULL;
	GList		*iter, *kept = NULL;
	gboolean	haveIncludes = FALSE;
	guint		rejected = 0, remembered = 0;

	static gint64	lastPrune = 0;

	if (!node || !items)
		return items;

	if (g_get_monotonic_time () - lastPrune > ENTRY_FILTER_PRUNE_INTERVAL || !lastPrune) {
		lastPrune = g_get_monotonic_time ();
		db_filtered_items_prune (ENTRY_FILTER_KEEP_DAYS);
	}

	for (GSList *f = entry_filter_get_all (); f; f = g_slist_next (f)) {
		entryFilterPtr filter = (entryFilterPtr)f->data;

		if (!filter->enabled || !filter->regex || !entry_filter_applies_to (filter, node))
			continue;

		applicable = g_slist_append (applicable, filter);
		if (ENTRY_FILTER_ACTION_INCLUDE_ONLY == filter->action)
			haveIncludes = TRUE;
	}

	if (!applicable)
		return items;

	for (iter = items; iter; iter = g_list_next (iter)) {
		itemPtr			item = (itemPtr)iter->data;
		g_autofree gchar	*key = entry_filter_get_item_key (item);
		g_autofree gchar	*content = NULL;
		gboolean		reject = FALSE, included = FALSE, markRead = FALSE, flag = FALSE;

		/* rejected before: drop without checking again */
		if (db_filtered_item_check (node->id, key)) {
			remembered++;
			item_unload (item);
			continue;
		}

		for (GSList *f = applicable; f && !reject; f = g_slist_next (f)) {
			entryFilterPtr filter = (entryFilterPtr)f->data;

			if (!entry_filter_matches (filter, item, &content))
				continue;

			switch (filter->action) {
				case ENTRY_FILTER_ACTION_EXCLUDE:
					debug (DEBUG_UPDATE, "entry filter \"%s\" rejects \"%s\"", filter->name, item_get_title (item));
					reject = TRUE;
					break;
				case ENTRY_FILTER_ACTION_INCLUDE_ONLY:
					included = TRUE;
					break;
				case ENTRY_FILTER_ACTION_MARK_READ:
					markRead = TRUE;
					break;
				case ENTRY_FILTER_ACTION_FLAG:
					flag = TRUE;
					break;
			}
		}

		if (!reject && haveIncludes && !included) {
			debug (DEBUG_UPDATE, "entry filters: \"%s\" matches no include filter", item_get_title (item));
			reject = TRUE;
		}

		if (reject) {
			rejected++;
			db_filtered_item_add (node->id, key);
			item_unload (item);
			continue;
		}

		if (markRead)
			item->readStatus = TRUE;
		if (flag)
			item->flagStatus = TRUE;

		kept = g_list_prepend (kept, item);
	}

	g_list_free (items);
	g_slist_free (applicable);

	if (rejected || remembered)
		debug (DEBUG_UPDATE, "entry filters for \"%s\": %u entries rejected, %u rejected earlier", node_get_title (node), rejected, remembered);

	return g_list_reverse (kept);
}
