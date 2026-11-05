/**
 * @file folder.h  sub folders for hierarchic node soures
 * 
 * Copyright (C) 2006-2024 Lars Windolf <lars.windolf@gmx.de>
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

#ifndef _FOLDER_H
#define _FOLDER_H

#include "node_provider.h"

/* 
   Liferea supports different node sources in the feed list tree.
   Some of those can be organized by sub folders implemented by 
   this node type. 
   
   The root node of the Liferea feed list tree is the implemented
   as a special folder and therefore shares almost all of the
   folder functionality. Being a read/write enabled folder it 
   allows to add other node sources as childs.
 */

/**
 * folder_get_update_interval: (skip)
 * @node:	a folder node
 *
 * Returns: the folder update schedule in minutes, -1 to inherit from
 * the parent folder (or the global default), -2 for no auto update
 */
gint folder_get_update_interval (Node *node);

/**
 * folder_set_update_interval: (skip)
 * @node:	a folder node
 * @interval:	schedule in minutes, -1 to inherit, -2 for no auto update
 */
void folder_set_update_interval (Node *node, gint interval);

/**
 * folder_get_override_children: (skip)
 * @node:	a folder node
 *
 * Returns: TRUE if the folder schedule wins over the schedules of
 * the feeds (and folders) inside
 */
gboolean folder_get_override_children (Node *node);

/**
 * folder_set_override_children: (skip)
 * @node:	a folder node
 * @override:	TRUE to let the folder schedule win over the feeds inside
 */
void folder_set_override_children (Node *node, gboolean override);

/**
 * folder_get_scheduled_interval: (skip)
 * @node:		the feed list node to resolve the schedule for
 * @ownInterval:	the node's own schedule (-1 inherit, -2 never, >0 minutes)
 * @folder: (out) (nullable): the folder whose schedule decided (or NULL)
 *
 * Applies the schedules of all parent folders to the node's own
 * schedule. The outermost folder overriding its children wins, then
 * the node's own schedule, then the nearest folder with a schedule.
 *
 * Returns: -1 (use the global default), -2 (no auto update) or minutes
 */
gint folder_get_scheduled_interval (Node *node, gint ownInterval, Node **folder);

/**
 * folder_describe_interval: (skip)
 * @interval:	schedule in minutes, -1 for the global default, -2 for never
 *
 * Returns: (transfer full): a translated text like "every 2 hours"
 */
gchar * folder_describe_interval (gint interval);

/**
 * folder_get_provider: (skip)
 */
nodeProviderPtr folder_get_provider (void);

/**
 * root_get_provider: (skip)
 */
nodeProviderPtr root_get_provider (void);

#endif
