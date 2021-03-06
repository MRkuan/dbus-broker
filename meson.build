project(
        'dbus-broker',
        'c',
        version: '3',
        license: 'Apache',
        default_options: [
                'c_std=c11',
        ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-DPACKAGE_VERSION=' + meson.project_version(), language: 'c')

cc = meson.get_compiler('c')
conf = configuration_data()
mod_pkgconfig = import('pkgconfig')

dep_cdvar = dependency('libcdvar', version: '>=1', fallback: [ 'c-dvar', 'libcdvar_dep' ])
dep_clist = dependency('libclist', version: '>=3', fallback: [ 'c-list', 'libclist_dep' ])
dep_crbtree = dependency('libcrbtree', version: '>=3', fallback: [ 'c-rbtree', 'libcrbtree_dep' ])
dep_csundry = dependency('libcsundry', version: '>=1', fallback: [ 'c-sundry', 'libcsundry_dep' ])
dep_dbus = dependency('dbus-1', version: '>=1.10', required: false)
dep_glib = dependency('glib-2.0', version: '>=2.50', required: false)
dep_libaudit = dependency('audit', version: '>=2.7.0', required: false)
dep_libselinux = dependency('libselinux', version: '>=2.5', required: false)
dep_libsystemd = dependency('libsystemd', version: '>=230', required: false)
dep_math = cc.find_library('m')
dep_systemd = dependency('systemd', required: false)
dep_thread = dependency('threads')
dep_expat = dependency('expat')

conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

if dep_systemd.found()
        conf.set('systemunitdir', dep_systemd.get_pkgconfig_variable('systemdsystemunitdir'))
        conf.set('userunitdir', dep_systemd.get_pkgconfig_variable('systemduserunitdir'))
endif

subdir('src')

if dep_systemd.found()
        subdir('test/dbus')
        subdir('units/system')
        subdir('units/user')
endif
