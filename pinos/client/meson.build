pinos_headers = [
  'array.h',
  'connection.h',
  'context.h',
  'interfaces.h',
  'introspect.h',
  'log.h',
  'loop.h',
  'map.h',
  'mem.h',
  'pinos.h',
  'properties.h',
  'protocol-native.h',
  'proxy.h',
  'rtkit.h',
  'sig.h',
  'stream.h',
  'subscribe.h',
  'thread-mainloop.h',
  'transport.h',
  'type.h',
  'utils.h',
]

pinos_sources = [
  'connection.c',
  'context.c',
  'introspect.c',
  'log.c',
  'loop.c',
  'mapper.c',
  'mem.c',
  'properties.c',
  'protocol-native.c',
  'proxy.c',
  'stream.c',
  'pinos.c',
  'rtkit.c',
  'thread-mainloop.c',
  'transport.c',
  'type.c',
  'utils.c',
]

install_headers(pinos_headers, subdir : 'pinos/client')

libpinos_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-D_POSIX_C_SOURCE',
]


libpinos = shared_library('pinos', pinos_sources,
  version : libversion,
  soversion : soversion,
  c_args : libpinos_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [dbus_dep, mathlib, pthread_lib],
)

pinos_dep = declare_dependency(link_with : libpinos,
  include_directories : [configinc, spa_inc],
  dependencies : [pthread_lib],
)
