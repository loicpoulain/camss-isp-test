project('camss-isp-m2m', 'c',
  version : '0.1',
  default_options : ['c_std=gnu11', 'warning_level=2'])

inc = include_directories('include')

gst_deps = []
gst_cargs = []
gst_sources = []
gst_dep = dependency('gstreamer-1.0', required : false)
gst_app_dep = dependency('gstreamer-app-1.0', required : false)
if gst_dep.found() and gst_app_dep.found()
  gst_deps = [gst_dep, gst_app_dep]
  gst_cargs = ['-DHAVE_GSTREAMER']
  gst_sources = ['src/gst_sink.c']
  message('GStreamer support enabled')
else
  message('GStreamer not found, -g option will not be available')
endif

executable('camss-isp-m2m',
  sources : [
    'src/main.c',
    'src/media.c',
    'src/isp_test.c',
    'src/params.c',
  ] + gst_sources,
  include_directories : inc,
  c_args : ['-D_GNU_SOURCE'] + gst_cargs,
  dependencies : gst_deps,
  install : true)
