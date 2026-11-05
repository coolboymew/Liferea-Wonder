/*
 * @file page_feed_dialog.h  setting up feeds made from web pages
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

#ifndef _PAGE_FEED_DIALOG_H
#define _PAGE_FEED_DIALOG_H

#include "subscription.h"

/**
 * page_feed_dialog_new: (skip)
 * @url: (nullable):	page address to start with
 *
 * Opens the dialog subscribing to a new page feed.
 */
void page_feed_dialog_new (const gchar *url);

/**
 * page_feed_dialog_edit: (skip)
 * @subscription:	a page feed subscription
 *
 * Opens the dialog changing the selectors of a page feed.
 */
void page_feed_dialog_edit (subscriptionPtr subscription);

#endif
