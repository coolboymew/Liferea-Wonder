/*
 * @file entry_filter_dialog.h  entry filter management dialogs
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

#ifndef _ENTRY_FILTER_DIALOG_H
#define _ENTRY_FILTER_DIALOG_H

#include "node.h"

/**
 * entry_filter_dialog_open: (skip)
 *
 * Opens the dialog listing all entry filters for adding, editing
 * and removing them. Changes are applied when saving the dialog.
 */
void entry_filter_dialog_open (void);

/**
 * entry_filter_dialog_new_for_node: (skip)
 * @node:	the feed or folder the new filter applies to
 *
 * Opens the filter editor for a new filter applying to @node only,
 * saving it right away on OK.
 */
void entry_filter_dialog_new_for_node (Node *node);

#endif
