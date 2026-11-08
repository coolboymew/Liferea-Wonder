# Liferea-Wonder

**AI slopped with Claude. Made mainly for CachyOS, but forked from the real thing. Been tested and in active use, but use at your own risk.**

Liferea-Wonder is a modified version of the [Liferea](https://github.com/lwindolf/liferea)
feed reader. Liferea is a desktop feed reader/news aggregator that brings
together all of the content from your favorite subscriptions into a simple
interface that makes it easy to organize and browse feeds. Its GUI is
similar to a desktop mail/news client, with an embedded web browser.

![screenshot](https://lzone.de/liferea/screenshots/screenshot2.png)

## New features

**Reading and updating feeds**

- **Per-site request delay:** requests to the same site are spaced out (2 s by default, adjustable in Preferences), so busy sites don't block you.
- **Folder update schedules:** a folder can have its own update interval for all its feeds; a feed can still override it.
- **Feeds from web pages:** turn any shop or list page into a feed by saying what an item, its title, link, picture and text are. With automatic detection, a live preview, first-seen dates for items, and kept cookies for sites with a language or age check.
- **Built-in browser loading for those pages:** pages built by JavaScript work; pass a site check once in a window ("Pass Site Check...", then *Check Passed*: the feed reads the page you see and its padlock goes); *Open Page* with *Detect Items Here* and *Pick an Item*; optional scrolling for a number of seconds, for lists that load more as you scroll.
- **Pick an Item, then say what's what:** point at one entry (a shop tile, a video), the arrow keys make the outlined block bigger or smaller, a box tells how many like it are on the page. After the click a panel shows what the title, text, price and picture would read; *Choose* and click the right part of the item instead, or *None*. Works with pages made of web components (custom elements, shadow DOM), like many video and shop sites.
- **Site check alerts:** a padlock frame in the tray icon's colour cycle and *Pass Site Check: feed* entries in the tray menu while feeds wait for a check, and optionally a KDE Connect phone message (both in Preferences, *Feeds*).
- **Cookie editor:** main menu *Cookies...* or right-click a feed, *Edit Cookies...*: see the cookies sites gave, change a value or keep a cookie until 2099 (e.g. an age check), add or delete cookies.
- **Prices for web page feeds:** give a *Price* selector (or let *Detect* / *Pick an Item* find it) and the item list shows a price column. When an item's price changes, its entry is updated: new price, a "Price changes" history in its text, the date of the change, and it is unread again (so tray colours and phone messages fire too).
- **Better feed discovery:** finds feeds that sites only link as ordinary links, then tries common feed addresses.
- **Name box in New Feed:** filled in automatically and kept after the first update.
- **RSS categories** are read (upstream drops them).

**Filtering**

- **Entry filters:** by word, phrase or pattern, in titles, text or categories. A filter can hide, show only matches, mark read or flag, for one feed or a whole folder. Right-click a feed: *Add Entry Filter...*
- **Rejected entries stay rejected:** remembered for 30 days after they leave the feed, so they don't come back.

**Tray icon** (with the *Tray Icon* plugin)

- **Colour alerts:** give any number of feeds one of six colours (each shown with its tray picture in the colour list). While such a feed has unread items, also older ones, at startup or marked unread by hand, the tray icon takes its colour within a second. With several colours on (and the site check padlock, see below) it shows each for 5 seconds and fades to the next. Preferences can limit colours to newly received entries.
- **Phone messages (KDE Connect):** per feed, *Send a message to my phone when new items arrive* (feed Properties, *Advanced*) pings every paired phone KDE Connect can reach; feeds updating at the same time share one message.
- **Close to tray:** closing the window hides it while the tray icon is shown; starting Liferea-Wonder again (e.g. a pinned launcher) brings the same window back.
- **Clicks:** left click shows the window or brings it to the front, right click opens the menu.
- **Reliable icon:** uses the application icon, which every icon theme has, and appears even when Liferea-Wonder starts before the panel.

**Looks, setup and safety**

- **Pictures fit the reading pane:** at most 75% of its height, keeping their proportions.
- **Import from regular Liferea:** feeds, database, icons and settings, with a backup of the previous data.
- **Own identity:** installs and runs next to regular Liferea without sharing anything.
- **Safer article view:** feed HTML is cleaned before it is shown, so scripts hidden in feeds cannot run.
- **Lighter page loads:** no video in any page of the built-in browser (also stopped video sites crashing it), no graphics card, 3D or sound for pages loaded in the background, and no camera or microphone for any page. A page that crashes anyway is loaded again in its window, and fails its background load at once instead of after a minute.
- **CachyOS / Arch packaging:** a PKGBUILD and an install guide.

**Fixed upstream bugs**

- The article view froze on some entries (title split over several tags) and used a full CPU core.
- Unread counts and tray alerts did not update for feeds updating in the background.
- Crash on "Mark all read" with nothing selected; crash when deleting a feed (right-click did not select it).
- Collapsed panes and a tiny window on first start.
- A crash when quitting before a window was opened.
- The preferences plugin, network setup running twice (cookies lost), SameSite cookies, dialogs blocking other windows, the missing window icon.

Details for each feature are further below, under "Feature details".

## Installing on CachyOS / Arch Linux

Install the build dependencies (once):

    sudo pacman -S --needed base-devel git meson intltool gobject-introspection \
        gtk4 libadwaita webkitgtk-6.0 libpeas-2 json-glib libxml2 libxslt sqlite \
        gsettings-desktop-schemas fribidi python-gobject

Get the source:

    git clone https://github.com/coolboymew/Liferea-Wonder.git
    cd Liferea-Wonder

**As a package (recommended)**, managed by pacman:

    cd packaging/archlinux
    makepkg -si                       # build, run the tests and install
    makepkg -sif                      # rebuild and reinstall after updating the source
    sudo pacman -R liferea-wonder     # uninstall

**Without packaging**, into `/usr/local`:

    meson setup build-system --prefix=/usr/local
    meson compile -C build-system
    meson test -C build-system
    sudo meson install -C build-system
    sudo ninja -C build-system uninstall     # uninstall

or just for your user, into `~/.local` (no sudo):

    meson setup build-user --prefix=$HOME/.local
    meson compile -C build-user
    meson install -C build-user
    ninja -C build-user uninstall            # uninstall

Each way uses its own build folder (`build-system`, `build-user`); to
start over, delete that folder. For the `~/.local` way, `~/.local/bin`
must be in your command path; in fish run `fish_add_path ~/.local/bin`
once, in bash or zsh add `export PATH="$HOME/.local/bin:$PATH"` to
`~/.bashrc` or `~/.zshrc`. Install only one of the three ways at a time:
with the package installed too, `liferea-wonder` may start the package's
copy, and a running Liferea-Wonder is only brought to the front, not
started again (quit it first from its tray menu or with Ctrl+Q).

Then start **Liferea-Wonder** from the application menu, or run
`liferea-wonder`. To show the tray icon, enable the *Tray Icon* plugin in
*Preferences → Plugins*. `packaging/INSTALL-CachyOS.md` covers installing a
built package on another computer.

## Moving your feeds and data

Liferea-Wonder keeps everything in three folders:

    ~/.config/liferea-wonder        feed list, entry filters, stylesheet, kept cookies
    ~/.local/share/liferea-wonder   database (all items, read and flag states),
                                    browser session of web page feeds (logins, site checks)
    ~/.cache/liferea-wonder         favicons (optional, they come back by themselves)

**From another computer:** quit Liferea-Wonder on both, then copy these
folders over. Settings (update interval, plugins, window layout...) can be
copied with:

    dconf dump /io/github/coolboymew/LifereaWonder/ > liferea-wonder-settings.txt   # old computer
    dconf load /io/github/coolboymew/LifereaWonder/ < liferea-wonder-settings.txt   # new computer

**From regular Liferea:** use *Subscriptions → Import From Liferea...* (or
start once with `liferea-wonder --import-from-liferea`). It copies the feed
list, the database, favicons, the user stylesheet and the settings both
versions share, after moving Liferea-Wonder's own data to a
`before-import-<date>` folder. Regular Liferea's files (`~/.config/liferea`,
`~/.local/share/liferea`) are only read. You can also copy those two
folders' contents into the Liferea-Wonder folders above by hand while both
programs are closed.

## About this fork

This is a modified fork of [Liferea](https://github.com/lwindolf/liferea)
by Lars Windolf and contributors, distributed under the same license, the
GNU General Public License version 2 or (at your option) any later version
(see `COPYING`). It is not affiliated with or endorsed by the upstream
project. It is based on upstream Liferea 2.0.3 (git commit `8c5ab3b4`);
the changes were made in November 2026, and
[CHANGED-FILES.md](CHANGED-FILES.md) lists every file added, changed,
renamed or removed compared to that upstream version.

It has its own identity, so it can be installed and run next to the
Liferea of your distribution without sharing anything: the command is
`liferea-wonder`, the menu entry "Liferea-Wonder", the app ID and settings
schema `io.github.coolboymew.LifereaWonder`, and the folders listed above.

## Feature details

- **Per-host request throttle.** Requests to the same host run one after
  another, spaced by a delay in seconds (fractions allowed, default 2,
  0 turns it off). Different hosts are still fetched in parallel.
  Set it in *Preferences → Feeds → Delay Between Requests to the Same Host*
  (GSettings key `host-request-delay`).
- **Folder update schedules.** *Properties* on a folder sets its update
  interval (inherit / every N minutes, hours or days / never). Feeds set to
  "use the folder or global default" take the nearest folder's schedule.
  With *Override the schedules of the feeds inside* checked the folder's
  schedule wins over every feed (and sub folder) inside it; the outermost
  overriding folder wins. Stored as `updateInterval` and
  `overrideChildIntervals` on the folder's outline in `feedlist.opml`.
  The feed properties dialog explains which folder schedule applies.
- **Entry filters.** *Tools → Entry Filters...* manages rules matching a whole
  word, a phrase or a regular expression (optionally case sensitive) in an
  entry's title, content, author, categories or link. Actions: exclude,
  only include (an entry must match one include filter), mark as read or
  flag. A filter applies to the checked feeds and folders (a folder covers
  everything inside), or on purpose to all feeds. Right-click a feed or
  folder and choose *Add Entry Filter...* to create a filter for just that
  feed or folder. Rejected entries are dropped before
  merging, so they are never stored and do not count toward the feed's item
  limit. Their GUIDs are remembered in the `filtered_items` DB table so they
  are not processed again; rows are refreshed while the entry is still in
  its feed and forgotten 30 days after it left (checked daily). Saving the
  filters forgets all remembered entries so they get checked against the
  new rules. Filters are stored in `~/.config/liferea/entry_filters.json`.
  RSS 2.0 `<category>` tags are now kept as item categories, so category
  filters work for RSS feeds too.
- **Name when subscribing.** The *New Feed* dialog has a *Name* box that is
  filled in with the feed's title as soon as the address is entered (website
  addresses are followed to their feed link). Change it right there to name
  the subscription yourself; the first update then keeps your name instead
  of replacing it with the feed's title. Leave it empty for the old
  behaviour.
  If no feed is found the box falls back to the page's title.
- **Deeper feed discovery.** When a website has no
  `<link rel="alternate">` feed tag, discovery also looks at ordinary links
  in the page whose address looks like a feed (`/feed`, `/feeds/...`,
  `/rss`, `*.rss`, `*.atom`, ...), as some sites only link their feed
  that way.
  If that finds nothing either, common feed addresses are tried one after
  another as a last resort (`feed`, `rss`, `feed.xml`, `rss.xml`,
  `atom.xml`, `index.xml`, `feeds/posts/default`, first in the page's
  folder, then at the site root), at most once per subscription and
  session, spaced by the host throttle. The first address that answers
  with a real feed becomes the subscription's source.
- **Feeds from web pages.** For pages listing things without a feed of their
  own (shop or search results), *Subscriptions → New Feed From Web Page...*
  (or *Web Page...* in the New Feed dialog) makes one. *Detect* loads the page
  and guesses which repeating block is an item; the selectors (CSS subset:
  tag, `.class`, `#id`, `[attribute]`, `[attribute=value]`, nested with
  spaces or `>`; or XPath) can be adjusted with a live *Preview* of the
  items found. Each item's link identifies it, so only items not seen before
  show up as new. *Keep cookies* stores cookies such a feed's site sets (in
  `kept_cookies.txt`, only for these feeds) and retries once after a
  language or age check page. Right-click a page feed to *Edit Web Page
  Feed...*. Settings are stored as `scrapeItem`, `scrapeTitle`, `scrapeLink`,
  `scrapeImage`, `scrapeText`, `keepCookies` and `loadInBrowser` on the
  feed's outline.

  *Load with the built-in browser* loads such a page in Liferea's browser
  engine instead of downloading it, so lists built by JavaScript can be read
  and the site sees a normal browser visit. These loads share one persistent
  browser session (`~/.local/share/liferea-wonder/page-browser`, separate from
  the item view). When a site shows a browser check (e.g. Cloudflare's "Just
  a moment..."), automatic checks are given a few seconds; if the check needs
  you, the feed gets a padlock icon and the right-click entry *Pass Site
  Check...* opens the page in a window of that session to do it once (or to
  confirm your age or log in). Once the page shows the site, click *Check
  Passed*: the feed reads that page as you see it (a new background load
  could get another check), the padlock goes and the window closes;
  clicking it while the page still looks like a check asks for a second
  click. Closing the window without it loads the feed again as before.
  Check pages are recognised by their title and the check's own page
  setup; the small Cloudflare script and Turnstile boxes that protected
  sites put into their normal pages no longer count (they kept the padlock
  on after the check was passed). Preferences, *Feeds*: *Show waiting site
  checks in the tray icon* (on by default: a padlock frame in the tray
  icon's colour cycle, the tooltip names the feeds, the tray menu gets *Pass Site Check:
  feed* entries for up to five) and *Send a KDE Connect message when a
  site check is needed* (off by default; one message when a feed starts
  waiting, stored as `site-check-tray` and `site-check-kdeconnect`). The
  page feed dialog has an *Open Page...* button for the same, whose window
  also offers *Detect Items Here* (detects on the page as you see it, at
  the address you are on) and *Pick an Item* (click one shop tile or list
  entry and the feed uses all items like it). While picking, the block
  under the mouse is outlined and a box at the bottom names it and how many
  like it the page has; Up/Down arrow pick a bigger or smaller block. The
  block is a repeated element (class or custom element name like
  `shop-item-card`) most of whose copies hold a link, so repeated
  labels or badges inside entries are not mistaken for entries; open
  shadow roots (web components) are looked into, and pages are read with
  their shadow content. The page's own click handling is held back while
  picking. After the click a panel lists Title, Text, Price and Picture
  with what each reads in the clicked entry: *Choose*, then click that
  part (a selector most other entries share is preferred), *None* leaves
  it out, *Use these* hands them to the feed dialog. Either one also turns on
  loading with the built-in browser, since the selectors then fit the page
  as the browser shows it. Only looking at the page and closing the window
  changes nothing about the feed (address, loading), the preview is just
  fetched again. Background loads wait until
  the item selector matches (pages loading their lists late). For pages that
  show more items as you scroll down, *Scroll down the page for N seconds*
  (up to 120, stored as `scrollSeconds`) scrolls the hidden page before
  reading it; the hidden view gets a 1280x1600 size and a small helper that
  stands in for the page timers browsers pause in hidden pages ("is this in
  view" and animation frames). Nothing tries to get around such checks. The last page the dialog looked at is kept in
  `~/.cache/liferea-wonder/page-feed-last.html` for working out selectors.
- **Pictures fit the view.** Pictures in articles are limited to the width
  of the reading pane and to 75% of its height, keeping their proportions,
  so they shrink and grow with the window.
- **Coloured tray icon alerts.** With the *Tray Icon* plugin enabled, a feed
  can be given a tray alert colour (blue, green, red, yellow, purple or
  white) in its properties (*Advanced* tab); the list shows each colour's
  tray picture. Any number of feeds can have colours. When changing between the plain and a coloured icon, the
  panel is told both the icon name and the picture changed (panels that
  keep what change notices tell them went on showing the plain icon).
  Several feeds may share a colour; the dialog names the feeds already
  using it. While such a feed has unread items the tray icon shows that
  colour: items that came with an update, older unread ones (at startup
  too) and items marked unread by hand all count, looked at every second;
  the tooltip lists the feeds. With several colours on, and the padlock
  frame while feeds wait for a site check, the icon shows each for 5
  seconds and fades to the next. Preferences, *Feeds*: *Tray colours only
  for newly received entries* (`tray-alert-new-only`, off by default)
  brings back the older behaviour: a colour only after an update brought
  new items, until one of the feed's items is read (kept over restarts
  as `trayAlertPending`). *KDE Connect messages only for newly received
  entries* (`kdeconnect-new-only`, on by default); off, feeds with phone
  messages also send one when they have unread items at startup or get
  some without an update (e.g. marked unread), once until they are all
  read again. A quiet alternative to pop-ups for
  people who keep notifications off. Stored as `trayAlert` on the feed's
  outline in `feedlist.opml`.
- **Prices in web page feeds.** The page feed dialog has a *Price*
  selector (stored as `scrapePrice`); *Detect* and *Pick an Item* look for
  a class with "price" or "cost". The price is kept with each entry
  (`price`) and shown right-aligned in the item list, like a column. Items
  are recognised by their link: when a known item comes with another
  price, its entry gets the new price, a history line per change (stored
  as `priceHistory`, shown under "Price changes:" newest first), the time
  of the change as its date, and it becomes unread again. Price changes
  count like new items for tray alerts, notifications and phone messages.
  Adding a price selector to an existing feed stores the prices of its
  known entries quietly (still read, same date); changes after that are
  news. Texts of neighbouring elements are separated by a space, so a
  sale price and the crossed out old one read "3,542 JPY 5,060".
- **Phone messages via KDE Connect.** Feed Properties → *Advanced* →
  *Send a message to my phone (KDE Connect) when new items arrive*
  (stored as `kdeConnect`). After an update with new or price-changed
  items, Liferea-Wonder waits 5 seconds for other feeds, then sends one
  ping message ("Liferea-Wonder: new items in Feed (2), Other feed (1)")
  over D-Bus to every paired, reachable device. Nothing happens without
  KDE Connect or a reachable phone.
- **Tray icon fixes.** The tray icon uses the application icon (the themed
  "emblem-web" is missing from icon themes like Breeze, the icon then did
  not show at all) and still appears when Liferea starts before the panel's
  tray, e.g. at login. While the icon is shown, closing the window hides it
  to the tray; starting Liferea again (a pinned launcher, the menu) shows
  the running one, *Quit* in the tray menu or Ctrl+Q quits. A left click
  on the icon brings the window up (shown when hidden, raised when behind
  other windows, using the activation token the panel sends on Wayland);
  the menu opens on a right click.
- **Fixed article view freezes** (upstream bug): removing a repeated
  title from an article looped forever when the title's text was split
  over several tags (e.g. a forum's label span followed by the rest), the
  view stopped showing anything and used a full CPU core until Liferea
  quit.
- **Safer article view** (upstream bug): feed HTML (descriptions,
  authors, comments, feed info) was put into the page unescaped and only
  cleaned afterwards, so event handlers in it such as `<img onerror>` ran
  briefly. The whole rendered page now goes through the bundled HTML
  cleaner (DOMPurify) before it is shown.
- **Cookie editor.** Main menu *Cookies...*, or right-click a feed loaded
  with the browser or keeping cookies, *Edit Cookies...* (shows that site's
  cookies). Lists the cookies of the browser loading session and the ones
  kept for feeds with *Keep cookies*, with value and how long they are
  kept. Click one to change its site, name, value, path or keep-until date
  (*Until 2099* for age or language checks, empty: until Liferea-Wonder
  quits), or delete it; *+* adds one to either store. The site's other
  cookie settings (secure, same-site) are kept.
- **Lighter page loads.** Page feeds loaded with the built-in browser run
  without graphics card acceleration, WebGL or sound when loading in the
  background; their visible window keeps WebGL and web audio (some site
  checks use them). No page ever plays video (video sites
  crashed the page right away on systems without GStreamer's base
  plugins), gets camera or microphone access. A crashed page is loaded
  again in its window (twice at most) and fails a background load at once. Pages already run in WebKit's sandbox (a
  separate, locked-down process that cannot read your files).
- **Fixed unread counts of feeds updating in the background** (upstream
  bug): only the feed shown in the item list got its unread count
  updated after new items arrived, others kept the old count until
  something recounted them, so with the window hidden neither the unread
  numbers nor the tray alerts changed. Feeds are now recounted after every
  update with new items.

## Notes from upstream Liferea

The sections below come from the original Liferea README and are about
regular Liferea, not Liferea-Wonder.

### Installation from Package

Many distributions have packaged Liferea:

[![Packages](https://repology.org/badge/latest-versions/liferea.svg)](https://repology.org/metapackage/liferea/versions)
[![Packages](https://repology.org/badge/tiny-repos/liferea.svg)](https://repology.org/metapackage/liferea/versions)

There is also a [flatpak package](https://github.com/flathub/net.sourceforge.liferea) available.

### Building

Compile with

    meson setup builddir && cd builddir
    meson compile
    meson test

Update translations

    meson compile liferea-update-po

Install

    meson install

If you compile with a --prefix directory which does not match $XDG_DATA_DIRS
you will get a runtime error about the schema not being found. To workaround
set $XDG_DATA_DIRS before starting Liferea. For example:

    env XDG_DATA_DIRS="$my_dir/share:$XDG_DATA_DIRS" $my_dir/bin/liferea

### Update JS Dependencies

    npm i
    npm audit fix
    npm run installDeps
