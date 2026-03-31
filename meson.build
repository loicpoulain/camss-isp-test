project('camss-isp-test', 'c',
  version : '0.1',
  default_options : ['c_std=gnu11', 'warning_level=2'])

inc = include_directories('include')

executable('camss-isp-test',
  sources : [
    'src/main.c',
    'src/media.c',
    'src/isp_test.c',
    'src/params.c',
  ],
  include_directories : inc,
  c_args : ['-D_GNU_SOURCE'],
  install : true)
