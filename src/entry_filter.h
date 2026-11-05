/*
 * @file entry_filter.h  dropping or tagging entries on feed update
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

#ifndef _ENTRY_FILTER_H
#define _ENTRY_FILTER_H

#include <glib.h>

#include "node.h"

/* Entry filters are user defined rules checked against every newly
   downloaded entry before it is merged into the feed's item set.
   Rejected entries are never stored, do not count toward the feed's
   cache limit and are remembered in the DB (filtered_items) so they
   are not processed again on later updates. */

typedef enum {
	ENTRY_FILTER_MATCH_WORD = 0,	/*<< whole word(s) */
	ENTRY_FILTER_MATCH_PHRASE,	/*<< text anywhere */
	ENTRY_FILTER_MATCH_REGEX	/*<< regular expression */
} entryFilterMatch;

typedef enum {
	ENTRY_FILTER_FIELD_TITLE	= (1<<0),
	ENTRY_FILTER_FIELD_CONTENT	= (1<<1),
	ENTRY_FILTER_FIELD_AUTHOR	= (1<<2),
	ENTRY_FILTER_FIELD_CATEGORY	= (1<<3),
	ENTRY_FILTER_FIELD_LINK		= (1<<4)
} entryFilterField;

typedef enum {
	ENTRY_FILTER_ACTION_EXCLUDE = 0,	/*<< drop matching entries */
	ENTRY_FILTER_ACTION_INCLUDE_ONLY,	/*<< drop entries not matching any include filter */
	ENTRY_FILTER_ACTION_MARK_READ,		/*<< keep, but mark matching entries as read */
	ENTRY_FILTER_ACTION_FLAG		/*<< keep, but flag matching entries */
} entryFilterAction;

typedef struct entryFilter {
	gchar		*id;		/*<< unique filter id */
	gchar		*name;		/*<< user visible name */
	gboolean	enabled;
	gchar		*pattern;	/*<< word, phrase or regular expression */
	entryFilterMatch match;
	gboolean	caseSensitive;
	guint		fields;		/*<< entryFilterField bitmask */
	entryFilterAction action;
	gboolean	allFeeds;	/*<< if TRUE applies to every feed */
	GSList		*nodeIds;	/*<< feed and folder ids the filter applies to (folders include their children) */

	GRegex		*regex;		/*<< compiled pattern (or NULL if invalid) */
} *entryFilterPtr;

/**
 * entry_filter_new: (skip)
 *
 * Returns: a new enabled filter matching words in titles and content
 */
entryFilterPtr entry_filter_new (void);

/**
 * entry_filter_copy: (skip)
 * @filter:	the filter to copy
 *
 * Returns: a deep copy (with a new compiled pattern)
 */
entryFilterPtr entry_filter_copy (entryFilterPtr filter);

/**
 * entry_filter_free: (skip)
 * @filter:	the filter to free
 */
void entry_filter_free (entryFilterPtr filter);

/**
 * entry_filter_compile: (skip)
 * @filter:	the filter
 * @error:	(out) (nullable): pattern errors
 *
 * (Re)compiles the filter pattern, to be called after changing
 * pattern, match type or case sensitivity.
 *
 * Returns: TRUE if the pattern is valid
 */
gboolean entry_filter_compile (entryFilterPtr filter, GError **error);

/**
 * entry_filter_get_all: (skip)
 *
 * Returns: (transfer none): the list of all filters (loaded on first use)
 */
GSList * entry_filter_get_all (void);

/**
 * entry_filter_set_all: (skip)
 * @filters:	(transfer full): the new list of filters
 *
 * Replaces all filters, saves them and forgets remembered rejected
 * entries so they are checked against the new rules on next update.
 */
void entry_filter_set_all (GSList *filters);

/**
 * entry_filter_applies_to: (skip)
 * @filter:	the filter
 * @node:	a feed list node
 *
 * Returns: TRUE if the filter applies to the node or one of its parents
 */
gboolean entry_filter_applies_to (entryFilterPtr filter, Node *node);

/**
 * entry_filter_apply: (skip)
 * @node:	the node the downloaded items belong to
 * @items:	(transfer full): list of downloaded items (itemPtr)
 *
 * Drops rejected entries (unloading them and remembering their keys)
 * and applies mark-read / flag actions to the kept ones.
 *
 * Returns: (transfer full): the list of kept items
 */
GList * entry_filter_apply (Node *node, GList *items);

#endif
