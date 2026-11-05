# Changed files

Liferea-Wonder is a modified version of [Liferea](https://github.com/lwindolf/liferea),
based on upstream Liferea 2.0.3 (git commit `8c5ab3b4`). As the GNU General
Public License asks, this lists every file added, changed, renamed or
removed compared to that version. All changes were made in November 2026;
README.md ("About this fork") describes what they do.

## Added (23)

- `CHANGED-FILES.md`
- `packaging/INSTALL-CachyOS.md`
- `packaging/archlinux/.gitignore`
- `packaging/archlinux/PKGBUILD`
- `resources/ui/entry_filter_edit.ui`
- `resources/ui/entry_filters.ui`
- `resources/ui/folder_properties.ui`
- `resources/ui/page_feed.ui`
- `src/entry_filter.c`
- `src/entry_filter.h`
- `src/install_import.c`
- `src/install_import.h`
- `src/kdeconnect.c`
- `src/kdeconnect.h`
- `src/parsers/page_scraper.c`
- `src/parsers/page_scraper.h`
- `src/tests/page_scraper.c`
- `src/ui/entry_filter_dialog.c`
- `src/ui/entry_filter_dialog.h`
- `src/ui/page_feed_dialog.c`
- `src/ui/page_feed_dialog.h`
- `src/webkit/page_browser.c`
- `src/webkit/page_browser.h`

## Changed (62)

- `.github/ISSUE_TEMPLATE/bug_report.md`
- `.gitignore`
- `README.md`
- `config.h.in`
- `css/liferea.css`
- `data/man/meson.build`
- `data/meson.build`
- `data/net.sourceforge.liferea.appdata.xml.in`
- `data/net.sourceforge.liferea.desktop.in`
- `data/net.sourceforge.liferea.service.in`
- `meson.build`
- `plugins/getfocus.py`
- `plugins/libnotify.py`
- `po/POTFILES`
- `resources/gresource.xml`
- `resources/js/helpers/render.js`
- `resources/js/htmlview.js`
- `resources/ui/about.ui`
- `resources/ui/mainwindow.ui`
- `resources/ui/prefs.ui`
- `resources/ui/properties.ui`
- `resources/ui/simple_subscription.ui`
- `src/actions/node_actions.c`
- `src/actions/shell_actions.c`
- `src/common.c`
- `src/conf.c`
- `src/conf.h`
- `src/db.c`
- `src/db.h`
- `src/feed_parser.c`
- `src/feed_parser.h`
- `src/html.c`
- `src/item_state.c`
- `src/itemset.c`
- `src/liferea_application.c`
- `src/meson.build`
- `src/metadata.c`
- `src/net.c`
- `src/node_providers/feed.c`
- `src/node_providers/folder.c`
- `src/node_providers/folder.h`
- `src/node_sources/default_source.c`
- `src/parsers/rss_item.c`
- `src/plugins/plugins_engine.c`
- `src/plugins/trayicon/stray.h`
- `src/plugins/trayicon/trayicon.c`
- `src/subscription.c`
- `src/subscription.h`
- `src/tests/test.c`
- `src/ui/feed_list_view.c`
- `src/ui/feed_list_view.h`
- `src/ui/icons.c`
- `src/ui/icons.h`
- `src/ui/item_list_view.c`
- `src/ui/liferea_shell.c`
- `src/ui/preferences_dialog.c`
- `src/ui/subscription_dialog.c`
- `src/update.c`
- `src/update.h`
- `src/update_job.c`
- `src/update_job.h`
- `src/update_job_queue.c`

## Renamed (8)

- `data/net.sf.liferea.gschema.xml` → `data/io.github.coolboymew.LifereaWonder.gschema.xml`
- `data/pixmaps/16x16/apps/net.sourceforge.liferea.png` → `data/pixmaps/16x16/apps/io.github.coolboymew.LifereaWonder.png`
- `data/pixmaps/22x22/apps/net.sourceforge.liferea.png` → `data/pixmaps/22x22/apps/io.github.coolboymew.LifereaWonder.png`
- `data/pixmaps/24x24/apps/net.sourceforge.liferea.png` → `data/pixmaps/24x24/apps/io.github.coolboymew.LifereaWonder.png`
- `data/pixmaps/32x32/apps/net.sourceforge.liferea.png` → `data/pixmaps/32x32/apps/io.github.coolboymew.LifereaWonder.png`
- `data/pixmaps/48x48/apps/net.sourceforge.liferea.png` → `data/pixmaps/48x48/apps/io.github.coolboymew.LifereaWonder.png`
- `data/pixmaps/scalable/apps/net.sourceforge.liferea-symbolic.svg` → `data/pixmaps/scalable/apps/io.github.coolboymew.LifereaWonder-symbolic.svg`
- `data/pixmaps/scalable/apps/net.sourceforge.liferea.svg` → `data/pixmaps/scalable/apps/io.github.coolboymew.LifereaWonder.svg`
