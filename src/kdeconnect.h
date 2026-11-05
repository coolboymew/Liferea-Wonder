/*
 * @file kdeconnect.h  telling paired phones about new items via KDE Connect
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

#ifndef _KDECONNECT_H
#define _KDECONNECT_H

#include <glib.h>

/* Feeds with "Send a message to my phone" set call this when an update
   brought new (or price changed) items. Feeds updating within a few
   seconds of each other share one message, sent as a KDE Connect ping
   to every paired and reachable device. Without KDE Connect running or
   without a reachable device nothing happens. */

/**
 * kdeconnect_notify_new_items: (skip)
 * @feedTitle:	the feed's name
 * @count:	number of new or changed items
 */
void kdeconnect_notify_new_items (const gchar *feedTitle, guint count);

#endif
