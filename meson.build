project('liferea-wonder', 'c',
  version: '2.0.2',
  meson_version: '>= 0.62.0'
)

add_project_arguments(
  '-Wno-analyzer-infinite-loop',
  language: 'c'
)

gnome = import('gnome')
i18n = import('i18n')

gettext_package = meson.project_name()
add_project_arguments('-DGETTEXT_PACKAGE=' + gettext_package, language: 'c')

subdir('po')

prefix = get_option('prefix')
datadir_base = join_paths(prefix, get_option('datadir'))
datadir      = join_paths(datadir_base, meson.project_name())
dbusservicesdir = join_paths(datadir_base, 'dbus-1', 'services')
girdir       = join_paths(datadir_base, meson.project_name(), 'gir-1.0')
libdir       = join_paths(prefix, get_option('libdir'), meson.project_name())
typelibdir   = join_paths(prefix, get_option('libdir'), meson.project_name(), 'girepository-1.0')
webprocessextensionsdir = join_paths(prefix, get_option('libdir'), meson.project_name(), 'web-process-extensions')
localedir    = join_paths(prefix, get_option('localedir'))

# This fork has its own identity so it can be installed next to upstream
# Liferea (e.g. the distribution package) without sharing any settings,
# data folders or D-Bus names: binary and folders are named after the
# project, the app ID and settings schema ID below.
app_id    = 'io.github.coolboymew.LifereaWonder'
schema_id = 'io.github.coolboymew.LifereaWonder'

conf = configuration_data()
conf.set_quoted('APPLICATION_ID', app_id)
conf.set_quoted('SCHEMA_ID', schema_id)
conf.set_quoted('USER_DIR_NAME', meson.project_name())
conf.set_quoted('BUILD_ROOT', meson.project_build_root())
conf.set_quoted('WEB_EXTENSIONS_DIR', webprocessextensionsdir)
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_URL', 'https://github.com/coolboymew/Liferea-Wonder')
conf.set_quoted('PACKAGE_DATA_DIR', datadir)
conf.set_quoted('PACKAGE_LIB_DIR', libdir)
conf.set_quoted('PACKAGE_LOCALE_DIR', localedir)
conf.set_quoted('VERSION', meson.project_version())
conf.set('ENABLE_NLS', 1)

liferea_deps = [
  dependency('glib-2.0', version: '>= 2.74.0'),
  dependency('gsettings-desktop-schemas'),
  dependency('gtk4', version: '>= 4.12.0'),
  dependency('libadwaita-1', version: '>=1.9'),
  dependency('json-glib-1.0', version: '>= 1.6'),
  dependency('libxml-2.0', version: '>= 2.6.12'),
  dependency('libxslt', version: '>= 1.1.19'),
  dependency('sqlite3', version: '>= 3.22'),
  dependency('webkitgtk-6.0', version: '>= 2.43.4'),
  dependency('webkitgtk-web-process-extension-6.0', version: '>= 2.43.4'),
  dependency('libpeas-2'),
  dependency('pango', version: '>= 1.56'),
  dependency('fribidi', version: '>= 0.19.7'),
  dependency('girepository-2.0'),
  dependency('dbus-1')
]

configure_file(
  input: 'config.h.in',
  output: 'config.h',
  configuration: conf
)

subdir('resources')
subdir('src')
subdir('data')

liferea = executable(meson.project_name(),
  liferea_sources + files('src/main.c'),
  liferea_resources,
  link_whole: plugins_static,
  dependencies: liferea_deps,
  include_directories: include_directories('.', 'src'),
  install: true
)
memcheck = find_program('src/tests/memcheck.sh')
testrunner = find_program('src/tests/run.sh')

# As tests use conf_*() methods we need to provide gsettings schemata
test_env = environment()
test_env.set(
  'GSETTINGS_SCHEMA_DIR',
  meson.project_build_root() / 'data'
)

# Note: the 'update' test is explicitly skipped as it's gsettings dependency breaks CI build (missing X11/DBUS)
foreach name: [
  'favicon',
  'net',
  'page_scraper',
  'parse_atom',
  'parse_date',
  'parse_enclosure',
  'parse_html',
  'parse_rss',
  'parse_uri',
  'parse_xml',
  'rule',
  'social',
  'subscription'
]
  test(
    name,
    testrunner,
    args: [liferea, '--test', name],
    env: test_env
  )
  test(
    'memcheck-' + name,
    memcheck,
    args: [name],
    env: test_env
  )
endforeach


install_subdir('plugins', install_dir: libdir)
install_subdir('css', install_dir: datadir)
install_subdir('opml', install_dir: datadir)
install_subdir('resources/help', install_dir: datadir)

dbus_service_file = configure_file(
  input: 'data/net.sourceforge.liferea.service.in',
  output: app_id + '.service',
  configuration: {
    'APP_ID': app_id,
    'LIFEREA_EXEC': join_paths(prefix, get_option('bindir'), meson.project_name()),
  }
)
install_data(dbus_service_file, install_dir: dbusservicesdir)

gir = gnome.generate_gir(liferea,
  namespace: 'Liferea',
  sources: liferea_gir_sources,
  nsversion: '3.0',
  includes: ['Gtk-4.0', 'libxml2-2.0', 'Json-1.0'],
  install: true,
  install_dir_gir: girdir,
  install_dir_typelib: typelibdir,
  extra_args: '--accept-unprefixed',
)

gnome.post_install(
  gtk_update_icon_cache: true,
  glib_compile_schemas: true,
  update_desktop_database: true
)
