project('flexalloc', 'c', default_options : ['c_std=c11'])
cc = meson.get_compiler('c')
pymod = import('python')
py = pymod.find_installation('python3', modules: ['pip', 'venv'], required: true)


add_project_arguments('-DFLEXALLOC_VERBOSITY=' + get_option('FLEXALLOC_VERBOSITY').to_string(), language : 'c')
if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif
if get_option('FLEXALLOC_XNVME_IGNORE_MDTS')
  add_project_arguments('-DFLEXALLOC_XNVME_IGNORE_MDTS', language : 'c')
endif

### Dependencies ###
xnvme_deps = dependency('xnvme', version : '>=0.2.0' )

### Files ###
libflexalloc_header_dirs = include_directories('./src')
flexalloc_util_files = ['src/flexalloc_util.c']
flexalloc_introspect = ['src/flexalloc_introspection.c']
xnvme_env_files = ['src/flexalloc_xnvme_env.c', 'src/flexalloc_xnvme_env.h']
flexalloc_znd_files = ['src/flexalloc_znd.c', 'src/flexalloc_znd.h']
flexalloc_common =  ['src/flexalloc.c', 'src/flexalloc_mm.c', 'src/flexalloc_hash.c', 'src/flexalloc_bits.c',
  'src/flexalloc_freelist.c', 'src/flexalloc_ll.c', 'src/flexalloc_ll.h', 'src/flexalloc_slabcache.c',
  xnvme_env_files, flexalloc_util_files, flexalloc_znd_files]

flexalloc_daemon_files = ['src/flexalloc_daemon_base.c']
libflexalloc_files = ['src/libflexalloc.c', flexalloc_common]

### Executables ###
executable('mkfs.flexalloc', ['src/flexalloc_mkfs.c', 'src/flexalloc_cli_common.c', flexalloc_common],
  dependencies: xnvme_deps, install : true)
executable('flexalloc_inspect', ['src/flexalloc_inspect.c', 'src/flexalloc_cli_common.c',
  flexalloc_introspect, flexalloc_common], dependencies: xnvme_deps)
daemon_exe = executable('flexalloc_daemon', ['src/flexalloc_daemon.c', flexalloc_common, 'src/flexalloc_cli_common.c', 'src/flexalloc_daemon_base.c', libflexalloc_files], dependencies: xnvme_deps)
executable('flexalloc_client', ['src/flexalloc_test_client.c', flexalloc_common, 'src/flexalloc_daemon_base.c', libflexalloc_files], dependencies: [xnvme_deps])

### Libraries ###
library = both_libraries('flexalloc', [libflexalloc_files, flexalloc_daemon_files],
                         dependencies: xnvme_deps, install : true)

foreach header_file: [
  'libflexalloc.h',
  'flexalloc_shared.h',
  'flexalloc_daemon_base.h',
  ]
  install_headers('src' / header_file)
endforeach

if get_option('fio_source_dir') != ''
  subdir('fio-ioengine')
else
  message('***  fio engine build: NO  ****\n\t `meson --reconfigure -Dfio_source_dir=<fio source dir> <build-dir>` to enable')
endif

### Tests ###
flexalloc_testing = ['tests/flexalloc_tests_common.c', 'tests/flexalloc_tests_common.h']
libflexalloc_t_files = ['src/libflexalloc_t.c', 'src/libflexalloc_t.h']

utils_tests = {
  #  'rt_hash'
  #: {'sources': 'tests/flexalloc_rt_hash.c',
  #   'is_parallel': true,
  #   'data_files' : ['wlist.txt'],
  #   'suite': 'utils'},
  'ut_hash'
  : {'sources': 'tests/flexalloc_ut_hash.c',
     'is_parallel': true,
     'suite': 'utils'},
  'ut_bits'
  : {'sources': 'tests/flexalloc_ut_bits.c',
     'is_parallel': true,
     'suite': 'utils'},
  'ut_freelist'
  : {'sources': 'tests/flexalloc_ut_freelist.c',
     'is_parallel': true,
     'suite': 'utils'}
}

xnvme_tests = {
  'from_to_storage'
  : {'sources': 'tests/flexalloc_rt_xnvme_to_from.c',
     'suite' : 'xnvme'}
}

core_tests = {
   'ut_slab'
   : {'sources': 'tests/flexalloc_ut_slab.c',
      'suite': 'core'},
  'rt_pool'
  : {'sources': 'tests/flexalloc_rt_pool.c',
     'suite': 'core'},
  'rt_object_allocate'
  : {'sources': 'tests/flexalloc_rt_object_allocate.c',
    'suite': 'core'},
  'rt_object_read_write'
  : {'sources': 'tests/flexalloc_rt_object_read_write.c',
    'suite' : 'core'},
  'rt_strp_object_read_write'
  : {'sources': 'tests/flexalloc_rt_strp_object_read_write.c',
    'suite' : 'core'},
  'rt_object_unaligned_write'
  : {'sources': 'tests/flexalloc_rt_object_unaligned_write.c',
    'suite' : 'core'},
  'rt_object_overread_overwrite'
  : {'sources': 'tests/flexalloc_rt_object_overread_overwrite.c',
    'suite' : 'core'},
  'rt_multi_pool_read_write'
  : {'sources': 'tests/flexalloc_rt_multi_pool_read_write.c',
    'suite' : 'core'}
}

lib_tests = {
  'rt_open_close'
  : {'sources': 'tests/flexalloc_rt_lib_open_close.c',
     'suite' : 'lib'}
  ,'rt_mkfs'
  : {'sources': 'tests/flexalloc_rt_mkfs.c',
     'suite' : 'lib'}
}

suites = [utils_tests, xnvme_tests, core_tests, lib_tests]
c_test_progs = []

foreach suite : suites
  foreach t_name, opts : suite
    assert('sources' in opts, 'error in "' + t_name
      + '" test entry must set key \'sources\' to a string or array of strings to C source files')

    t_deps = xnvme_deps
    t_sources = opts.get('sources', [])
    t_opts = {'timeout': 60, 'is_parallel': false} + opts.get('test_opts', {})
    t_suite = opts.get('suite', 'Default')
    t_link_args = opts.get('link_args', [])
    t_exec = 'test_' + opts.get('executable', t_name)
    t_exec_files = opts.get('exec_files', [flexalloc_common, flexalloc_testing, libflexalloc_files])
    t_data_files = opts.get('data_files', [])

    if t_data_files.length() > 0
      foreach data_file : t_data_files
        configure_file(input: 'tests/'+data_file, output : data_file, copy : 'true')
      endforeach
    endif
    prog = executable(t_exec, [t_exec_files, t_sources],
      dependencies: t_deps, link_args : t_link_args, include_directories : 'src')
    c_test_progs += prog

    test_data = configuration_data()
    test_data.set('flex_alloc_test_executable_name', t_exec)
    configure_file(input : './tests/flexalloc_pyt.py.in',
      output : t_exec + '.py',
      configuration : test_data)
  endforeach
endforeach

## Python Bindings && wheel ##
tgt_build_py_bindings = custom_target(
  'build_py_bindings',
  output: 'dummy',
  build_always: true,
  command: [
    meson.current_source_dir() + '/scripts/build_python_bindings.sh',
  ],
  env: {
    'MESON_SOURCE_ROOT': meson.current_source_dir(),
    'MESON_BUILD_ROOT': meson.current_build_dir(),
    'PYTHON_BIN': py.full_path()
  },
  depends: [library],
)

run_target(
  'run-tests',
  command: [
    meson.current_source_dir() + '/scripts/run_tests.sh',
  ],
  depends: [tgt_build_py_bindings, c_test_progs, daemon_exe],
  env: {
    'MESON_SOURCE_ROOT': meson.current_source_dir(),
    'MESON_BUILD_ROOT': meson.current_build_dir(),
    'PYTHON_BIN': py.full_path()
  })
