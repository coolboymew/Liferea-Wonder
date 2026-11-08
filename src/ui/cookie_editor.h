/**
 * @file cookie_editor.h  looking at and changing kept cookies
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

#ifndef _COOKIE_EDITOR_H
#define _COOKIE_EDITOR_H

#include <glib.h>

/**
 * cookie_editor_open: (skip)
 * @filter: (nullable):	site to show the cookies of (e.g. "example.com"),
 *			NULL for all
 *
 * Opens the window listing the cookies sites gave to Liferea-Wonder:
 * those of the browser loading (page feeds loaded with the built-in
 * browser, site checks) and those kept for feeds with "Keep cookies".
 * Values and expiry dates can be changed (e.g. an age check cookie kept
 * until 2099), cookies added and deleted.
 */
void cookie_editor_open (const gchar *filter);

#endif
