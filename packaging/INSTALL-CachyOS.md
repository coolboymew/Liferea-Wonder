# Installing Liferea-Wonder on CachyOS

This bundle holds everything needed to install Liferea-Wonder on another
CachyOS (or Arch Linux based) computer:

| File | What it is |
| --- | --- |
| `liferea-wonder-2.0.3.r9.g8c5ab3b4-1-x86_64.pkg.tar.zst` | The ready-built package, installed with pacman |
| `liferea-wonder-source.tar.gz` | The complete source with its CachyOS package recipe, to rebuild if ever needed |
| `INSTALL-CachyOS.md` | This guide |

Liferea-Wonder runs next to the regular Liferea from the CachyOS repositories
without sharing anything: its command is `liferea-wonder`, its menu entry
"Liferea-Wonder", and its data lives in `~/.config/liferea-wonder`,
`~/.local/share/liferea-wonder` and `~/.cache/liferea-wonder`.


## 1. Install the ready-built package (recommended)

Unpack the zip, open a terminal in the unpacked folder and run:

    sudo pacman -U liferea-wonder-2.0.3.r9.g8c5ab3b4-1-x86_64.pkg.tar.zst

pacman installs the libraries it needs (GTK 4, libadwaita, WebKitGTK 6,
libpeas 2, ...) from the CachyOS repositories by itself and asks before doing
so. Update the system first if it has not been updated for a while
(`sudo pacman -Syu`), so the package finds the library versions it was built
against.

Optional extras for two plug-ins:

    sudo pacman -S --needed libnotify python-requests

Then start **Liferea-Wonder** from the application menu, or run
`liferea-wonder` in a terminal.

If the package refuses to install or Liferea-Wonder does not start after a
big system update, rebuild it from the source (section 2).


## 2. Rebuild from the source (if the package does not fit)

Install the build tools and libraries once:

    sudo pacman -S --needed base-devel meson intltool gobject-introspection \
        gtk4 libadwaita webkitgtk-6.0 libpeas-2 json-glib libxml2 libxslt sqlite \
        gsettings-desktop-schemas fribidi python-gobject

Unpack the source and build the package (this also runs the test suite):

    tar xzf liferea-wonder-source.tar.gz
    cd liferea-wonder-source/packaging/archlinux
    makepkg -si

`makepkg -si` builds, tests and installs it in one go. After changes to the
source, `makepkg -sif` rebuilds and reinstalls.


## 3. Bring your feeds along

**From another Liferea-Wonder:** close Liferea-Wonder on both computers, then
copy these folders from the old computer's home folder to the new one's:

    ~/.config/liferea-wonder          feed list, entry filters, stylesheet, kept cookies
    ~/.local/share/liferea-wonder     database (all items, read and flag states),
                                    browser session of page feeds (passed site checks)

`~/.cache/liferea-wonder` (favicons) is optional, it fills itself again.
Settings such as the window size, update interval and tray plug-in are not in
these folders; set them again in Preferences, or copy them with:

    dconf dump /io/github/coolboymew/LifereaWonder/ > liferea-wonder-settings.txt     # old computer
    dconf load /io/github/coolboymew/LifereaWonder/ < liferea-wonder-settings.txt     # new computer

These folders hold private data (cookies of sites with page feeds), keep the
copy to yourself.

**From the regular Liferea on the same computer:** in Liferea-Wonder use
*Subscriptions → Import From Liferea...* (or start once with
`liferea-wonder --import-from-liferea`). It copies the feed list, database,
favicons, stylesheet and shared settings; Liferea-Wonder's own previous data
is kept in a `before-import-<date>` folder in `~/.local/share/liferea-wonder`.

**Only the feed list:** *Subscriptions → Export Feed List...* on the old
computer and *Import Feed List...* on the new one. Web page feed settings come
along, items and read states do not.


## 4. First steps

- **Automatic updates:** Preferences → Feeds → *Default Feed Refresh
  Interval* (e.g. 1 hour). The default is "never", which only updates at
  startup or by hand. Folders and feeds can have their own schedule in their
  Properties.
- **Polite to websites:** Preferences → Feeds → *Delay Between Requests to the
  Same Host* (default 2 seconds).
- **Tray icon colours:** enable the *Tray Icon* plug-in in Preferences →
  Plugins, then give feeds a colour in their Properties → Advanced.
- **Web page feeds** (shops and other pages without a feed): *Subscriptions →
  New Feed From Web Page...*. For pages built by JavaScript or with a browser
  check, tick *Load with the built-in browser* and use *Open Page...*, then
  *Detect Items Here* or *Pick an Item* in that window. A padlock icon on a
  feed means its site wants the browser check passed again: right-click →
  *Pass Site Check...*.
- **Entry filters:** right-click a feed or folder → *Add Entry Filter...*, or
  *Tools → Entry Filters...* to manage all of them.

The README inside the source describes every addition in detail.


## 5. Remove it

    sudo pacman -R liferea-wonder

Your feeds and settings stay in the folders listed above; delete them too for
a clean removal:

    rm -rf ~/.config/liferea-wonder ~/.local/share/liferea-wonder ~/.cache/liferea-wonder
    dconf reset -f /io/github/coolboymew/LifereaWonder/
