project('CM4all beng-proxy', ['c', 'cpp'], version: '17.0.46',
  meson_version: '>= 0.47',
  default_options: [
    'c_std=c11',
    'cpp_std=c++2a',
    'warning_level=3',

    # This is only here to build subprojects as static libraries
    'default_library=static',
  ],
)

version_h = configuration_data()
version_h.set_quoted('PACKAGE', meson.project_name())
version_h.set_quoted('VERSION', meson.project_version())
configure_file(output: 'version.h', configuration: version_h)

lb_features_config = configuration_data()
lb_features_config.set('ENABLE_CERTDB', get_option('certdb'))
configure_file(output: 'lb_features.h', configuration: lb_features_config)

compiler = meson.get_compiler('cpp')
c_compiler = meson.get_compiler('c')

pkg_mod = import('pkgconfig')
pkg_mod.generate(version: meson.project_version(),
                 name: 'libcm4all-beng-proxy3',
                 description: 'CM4all BENG proxy library',
                 install_dir: join_paths(get_option('datadir'), 'pkgconfig'))

debug = get_option('b_ndebug') != 'true'

common_flags = [
  '-D_REENTRANT', '-D_GNU_SOURCE',
  '-DCACHE_LOG',
  '-Wwrite-strings', '-Wcast-qual', '-Wcast-align', '-Wfloat-equal',
  '-Wshadow', '-Wpointer-arith',
  '-Wmissing-declarations', '-Wmissing-noreturn', '-Wmissing-format-attribute',
  '-Wredundant-decls',
  '-Wunused',
  '-Wno-unused-result',
  '-Wundef',
  '-Wvla',
]

test_common_flags = [
  '-fno-omit-frame-pointer',
  '-fvisibility=hidden',
]

add_global_link_arguments(
  compiler.get_supported_link_arguments(
    # make relocations read-only (hardening)
    '-Wl,-z,relro',

    # no lazy binding, please - not worth it for a daemon
    '-Wl,-z,now',
  ),
  language: 'cpp'
)

if not debug
  test_common_flags += [
    '-ffunction-sections',
    '-fdata-sections',
  ]

  add_global_link_arguments(
    compiler.get_supported_link_arguments(
      '-Wl,--gc-sections',
      '-Wl,--icf=all',
    ),
    language: 'cpp'
  )
endif

test_cxxflags = test_common_flags + [
  '-fno-threadsafe-statics',
  '-fmerge-all-constants',

  '-Wno-missing-field-initializers',
  '-Wno-non-virtual-dtor',

  # the only warnings we got from this are from formatted error
  # messages, and their truncation is harmless
  '-Wno-format-truncation',
]

test_cflags = test_common_flags + [
  '-Wmissing-prototypes', '-Wstrict-prototypes',
  '-Wbad-function-cast',
  '-Waggregate-return',
  '-Wnested-externs',
]

add_global_arguments(common_flags, language: 'c')
add_global_arguments(common_flags, language: 'cpp')

add_global_arguments(compiler.get_supported_arguments(test_cxxflags), language: 'cpp')
add_global_arguments(c_compiler.get_supported_arguments(test_cflags), language: 'c')

if debug
  debug_flags = ['-DPOISON', '-DTRACE']
  debug_flags += ['-DDEBUG_POOL_REF']
else
  debug_flags = []
endif

stopwatch = get_option('stopwatch')
if stopwatch
  debug_flags += ['-DENABLE_STOPWATCH']
endif

if compiler.has_header('valgrind/memcheck.h')
  add_global_arguments('-DHAVE_VALGRIND_MEMCHECK_H', language: 'cpp')
  add_global_arguments('-DHAVE_VALGRIND_MEMCHECK_H', language: 'c')

  if debug
    debug_flags += ['-DVALGRIND']
  endif
endif

add_global_arguments(debug_flags, language: 'c')
add_global_arguments(debug_flags, language: 'cpp')

if get_option('coroutines')
  if compiler.get_id() == 'clang'
    add_global_arguments(
      '-fcoroutines-ts',
      '-stdlib=libc++',
      '-D__cpp_impl_coroutine',
      language: 'cpp')
    add_global_link_arguments('-stdlib=libc++', language: 'cpp')
  else
    add_global_arguments('-fcoroutines', language: 'cpp')
  endif
else
  error('Coroutines must be enabled')
endif

if get_option('static_libcxx')
  link_args = ['-static-libstdc++']

  if compiler.get_id() == 'clang'
    # clang uses only "-lc++" when "-static-libstdc++" is used, but we
    # also need "-lc++abi" and "-pthread"
    link_args += ['-Wl,-Bstatic,-lc++,-lc++abi,-Bdynamic', '-pthread']
  endif

  libcxx = declare_dependency(link_args: link_args)
else
  libcxx = dependency('', required: false)
endif

threads = dependency('threads')
zlib = dependency('zlib')
libcrypt = compiler.find_library('crypt')

inc = include_directories('src', 'include', 'libcommon/src', '.')

subdir('libcommon/src/util')
subdir('libcommon/src/uri')

util2 = static_library(
  'util2',
  'src/util/LimitedConcurrencyQueue.cxx',
  'src/util/StringSet.cxx',
  'src/uri/Base.cxx',
  'src/uri/Compare.cxx',
  'src/uri/Dissect.cxx',
  'src/uri/Recompose.cxx',
  'src/uri/Extract.cxx',
  'src/uri/Relative.cxx',
  'src/escape_html.cxx',
  'src/escape_css.cxx',
  include_directories: inc,
)
util_dep = declare_dependency(
  link_with: util2,
  dependencies: [
    util_dep,
    uri_dep,
  ],
)

subdir('libcommon/src/pcre')
subdir('libcommon/src/sodium')
subdir('libcommon/src/time')
subdir('libcommon/src/io')

io2 = static_library(
  'io2',
  'src/io/Buffered.cxx',
  'src/io/StringFile.cxx',
  'src/io/SpliceSupport.cxx',
  'src/io/StatAt.cxx',
  include_directories: inc,
)
io_dep = declare_dependency(
  link_with: io2,
  dependencies: [
    io_dep,
  ],
)

subdir('libcommon/src/io/uring')
subdir('libcommon/src/system')

system2 = static_library(
  'system2',
  'src/system/SetupProcess.cxx',
  'src/system/Isolate.cxx',
  include_directories: inc,
)
system_dep = declare_dependency(
  link_with: system2,
  dependencies: [
    system_dep,
  ],
)

subdir('libcommon/src/http')
subdir('libcommon/src/json')
subdir('libcommon/src/lua')

memory = static_library('memory',
  'src/fb_pool.cxx',
  'src/SlicePool.cxx',
  'src/SliceAllocation.cxx',
  'src/SliceFifoBuffer.cxx',
  'src/MultiFifoBuffer.cxx',
  'src/DefaultFifoBuffer.cxx',
  'src/DefaultChunkAllocator.cxx',
  'src/GrowingBuffer.cxx',
  'src/rubber.cxx',
  include_directories: inc,
  dependencies: [
  ])
memory_dep = declare_dependency(link_with: memory,
                               dependencies: [system_dep])

pool = static_library('pool',
  'src/AllocatorPtr.cxx',
  'src/pool/Ptr.cxx',
  'src/pool/RootPool.cxx',
  'src/pool/tpool.cxx',
  'src/pool/pstring.cxx',
  'src/pool/pool.cxx',
  'src/pool/LeakDetector.cxx',
  include_directories: inc,
)
pool_dep = declare_dependency(link_with: pool,
                             dependencies: [memory_dep, io_dep])

putil = static_library('putil',
  'src/expansible_buffer.cxx',
  'src/strmap.cxx',
  'src/PStringSet.cxx',
  'src/puri_edit.cxx',
  'src/puri_escape.cxx',
  'src/puri_relative.cxx',
  'src/escape_pool.cxx',
  include_directories: inc,
  dependencies: [
  ])
putil_dep = declare_dependency(link_with: putil,
  dependencies: [
    pool_dep,
  ],
)

subdir('libcommon/src/event')

# Utility library using libevent
eutil = static_library('eutil',
  'src/cache.cxx',
  include_directories: inc,
)
eutil_dep = declare_dependency(link_with: eutil,
                              dependencies: [event_dep])

subdir('libcommon/src/net')

net2 = static_library(
  'net2',
  'src/net/FailureManager.cxx',
  'src/net/FailureInfo.cxx',
  'src/net/FailureRef.cxx',
  'src/net/TempListener.cxx',
  include_directories: inc,
)
net_dep = declare_dependency(
  link_with: net2,
  dependencies: [
    net_dep,
  ],
)

subdir('libcommon/src/event/net')
subdir('libcommon/src/event/uring')

event_net2 = static_library(
  'event_net2',
  'src/net/PConnectSocket.cxx',
  'src/net/Ping.cxx',
  'src/net/UdpDistribute.cxx',
  include_directories: inc,
)
event_net_dep = declare_dependency(
  link_with: event_net2,
  dependencies: [
    event_net_dep,
    memory_dep,
  ],
)

if stopwatch
  stopwatch = static_library('stopwatch',
    'src/istream_stopwatch.cxx',
    'src/stopwatch.cxx',
    include_directories: inc,
  )
  stopwatch_dep = declare_dependency(link_with: stopwatch,
                                    dependencies: [net_dep])
else
  stopwatch_dep = declare_dependency()
endif

subdir('libcommon/src/stock')

stock2 = static_library(
  'stock2',
  'src/stock/MultiStock.cxx',
  'src/stock/Lease.cxx',
  include_directories: inc,
)
stock_dep = declare_dependency(
  link_with: stock2,
  dependencies: [stock_dep, event_dep],
)

subdir('libcommon/src/ssl')
subdir('libcommon/src/jwt')

ssl2_sources = []

if get_option('certdb')
  subdir('libcommon/src/pg')

  ssl2_sources += [
    'src/certdb/CertDatabase.cxx',
    'src/certdb/FromResult.cxx',
    'src/certdb/WrapKey.cxx',
    'src/certdb/Wildcard.cxx',
    'src/ssl/Cache.cxx',
    'src/ssl/NameCache.cxx',
    'src/ssl/DbSniCallback.cxx',
  ]
else
  pg_dep = declare_dependency(required: false)
endif

ssl2 = static_library(
  'ssl2',
  'src/ssl/Basic.cxx',
  'src/ssl/Client.cxx',
  'src/ssl/Factory.cxx',
  'src/ssl/AlpnSelect.cxx',
  'src/ssl/AlpnEnable.cxx',
  'src/ssl/FifoBufferBio.cxx',
  'src/ssl/Filter.cxx',
  'src/ssl/Init.cxx',
  ssl2_sources,
  include_directories: inc,
  dependencies: [
    pg_dep,
  ],
)
ssl_dep = declare_dependency(
  link_with: ssl2,
  dependencies: [
    ssl_dep,
  ],
)

libcommon_require_uring = true

libcommon_require_avahi = get_option('zeroconf')
subdir('libcommon/src/avahi')
if avahi_dep.found()
  avahi_dep = declare_dependency(compile_args: '-DHAVE_AVAHI',
                                 dependencies: avahi_dep)
endif

if get_option('certdb')
  subdir('libcommon/src/curl')
endif

libcommon_enable_libsystemd = get_option('systemd')
if libcommon_enable_libsystemd or avahi_dep.found()
  subdir('libcommon/src/odbus')
endif

subdir('src/istream')

expand = static_library('expand',
  'src/regex.cxx',
  'src/pexpand.cxx',
  include_directories: inc,
  dependencies: [
    pcre_dep,
  ])
expand_dep = declare_dependency(link_with: expand,
                                dependencies: [putil_dep])

adata = static_library('adata',
  'libcommon/src/adata/ExpandableStringList.cxx',
  include_directories: inc,
  dependencies: [
  ])
adata_dep = declare_dependency(link_with: adata)

subdir('libcommon/src/spawn')

spawn_dep = declare_dependency(
  dependencies: [
    spawn_dep,
    expand_dep,
  ],
)

istream_spawn = static_library(
  'istream_spawn',
  'src/spawn/IstreamSpawn.cxx',
  include_directories: inc,
)
istream_spawn_dep = declare_dependency(
  link_with: istream_spawn,
  dependencies: [
    spawn_dep,
    istream_dep,
  ],
)

cluster = static_library(
  'cluster',
  'src/cluster/AddressSticky.cxx',
  'src/cluster/BalancerMap.cxx',
  'src/cluster/FailureManagerProxy.cxx',
  include_directories: inc,
  dependencies: [
    sodium_dep,
  ],
)
cluster_dep = declare_dependency(
  link_with: cluster,
  dependencies: [],
)

raddress = static_library('raddress',
  'src/regex.cxx',
  'src/cgi/Address.cxx',
  'src/file_address.cxx',
  'src/delegate/Address.cxx',
  'src/nfs/Address.cxx',
  'src/lhttp_address.cxx',
  'src/http/Address.cxx',
  'src/cluster/AddressList.cxx',
  'src/ResourceAddress.cxx',
  include_directories: inc,
  dependencies: [
    libpcre,
    sodium_dep,
  ])
raddress_dep = declare_dependency(
  link_with: raddress,
  dependencies: [
    net_dep,
    spawn_dep,
    adata_dep,
    expand_dep,
    pcre_dep,
  ],
)

subdir('src/was')

http_util = static_library('http_util',
  'src/http/Tokenizer.cxx',
  'src/http/PTokenizer.cxx',
  'src/http/Quote.cxx',
  'src/http/Upgrade.cxx',
  'src/http/PList.cxx',
  'src/http/PHeaderUtil.cxx',
  'src/http/HeaderUtil.cxx',
  'src/http/HeaderParser.cxx',
  'src/http/HeaderWriter.cxx',
  include_directories: inc,
)
http_util_dep = declare_dependency(link_with: http_util,
                                   dependencies: [pool_dep, putil_dep, http_dep])

cookie = static_library('cookie',
  'src/http/CookieJar.cxx',
  'src/http/CookieClient.cxx',
  'src/http/CookieServer.cxx',
  'src/http/CookieString.cxx',
  include_directories: inc,
)
cookie_dep = declare_dependency(link_with: cookie,
                                dependencies: [http_util_dep, putil_dep])

thread_pool = static_library('thread_queue',
  'src/thread/Queue.cxx',
  'src/thread/Worker.cxx',
  'src/thread/Pool.cxx',
  'src/thread/Notify.cxx',
  include_directories: inc,
  dependencies: [
    event_dep,
    eutil_dep,
  ])
thread_pool_dep = declare_dependency(
  link_with: thread_pool,
  dependencies: [
    event_dep,
    eutil_dep,
  ],
)

socket = static_library('socket',
  'src/fs/FilteredSocket.cxx',
  'src/fs/Ptr.cxx',
  'src/fs/NopSocketFilter.cxx',
  'src/fs/NopThreadSocketFilter.cxx',
  'src/fs/ThreadSocketFilter.cxx',
  'src/fs/Listener.cxx',
  'src/fs/Connect.cxx',
  'src/fs/Lease.cxx',
  'src/fs/Stock.cxx',
  'src/fs/Key.cxx',
  'src/fs/Balancer.cxx',
  include_directories: inc,
)
socket_dep = declare_dependency(link_with: socket,
                               dependencies: [event_net_dep])

subdir('libcommon/src/translation')
subdir('libcommon/src/translation/server')

libcommon_translation_dep = translation_dep

translation2_sources = []
if get_option('coroutines')
  translation2_sources += 'src/translation/CoTranslate.cxx'
endif

translation2 = static_library(
  'translation2',
  'src/translation/Builder.cxx',
  'src/translation/Multi.cxx',
  'src/translation/Cache.cxx',
  'src/translation/Stock.cxx',
  'src/translation/Layout.cxx',
  'src/translation/Marshal.cxx',
  'src/translation/Client.cxx',
  'src/translation/Transformation.cxx',
  'src/translation/FilterTransformation.cxx',
  'src/translation/SubstTransformation.cxx',
  'src/translation/Vary.cxx',
  'src/translation/InvalidateParser.cxx',
  translation2_sources,
  include_directories: inc,
)

translation_dep = declare_dependency(
  link_with: translation2,
  dependencies: [
    translation_dep,
    event_dep,
    putil_dep,
    eutil_dep,
    raddress_dep,
    socket_dep,
    stopwatch_dep
  ],
)

http_common = static_library('http_common',
  'src/http_body.cxx',
  include_directories: inc,
)
http_common_dep = declare_dependency(link_with: http_common,
                                    dependencies: [istream_dep, io_dep])

http_client = static_library('http_client',
  'src/http_client.cxx',
  include_directories: inc,
)
http_client_dep = declare_dependency(
  link_with: http_client,
  dependencies: [
    socket_dep,
    putil_dep,
    http_util_dep,
    http_common_dep,
    stopwatch_dep,
  ],
)

http_server = static_library('http_server',
  'src/http/IncomingRequest.cxx',
  'src/http_server/Request.cxx',
  'src/http_server/http_server.cxx',
  'src/http_server/http_server_send.cxx',
  'src/http_server/http_server_request.cxx',
  'src/http_server/http_server_read.cxx',
  'src/http_server/http_server_response.cxx',
  include_directories: inc,
)
http_server_dep = declare_dependency(
  link_with: http_server,
  dependencies: [
    http_util_dep,
    http_common_dep,
    putil_dep,
    socket_dep,
    stopwatch_dep,
  ],
)

subdir('src/nghttp2')

http_cache = static_library('http_cache',
  'src/http_cache.cxx',
  'src/http_cache_document.cxx',
  'src/http_cache_age.cxx',
  'src/http_cache_heap.cxx',
  'src/http_cache_item.cxx',
  'src/http_cache_info.cxx',
  'src/http_cache_rfc.cxx',
  include_directories: inc,
)
http_cache_dep = declare_dependency(
  link_with: http_cache,
  dependencies: [
    eutil_dep,
    http_util_dep,
    istream_dep,
    raddress_dep,
    stopwatch_dep,
  ],
)

cgi = static_library('cgi',
  'src/cgi/Glue.cxx',
  'src/cgi/Parser.cxx',
  'src/cgi/Client.cxx',
  'src/cgi/Launch.cxx',
  include_directories: inc,
)
cgi_dep = declare_dependency(
  link_with: cgi,
  dependencies: [
    stopwatch_dep,
    istream_spawn_dep,
    http_util_dep,
  ],
)

fcgi_client = static_library('fcgi_client',
  'src/fcgi/Client.cxx',
  'src/fcgi/Remote.cxx',
  'src/fcgi/Request.cxx',
  'src/fcgi/Serialize.cxx',
  'src/fcgi/Stock.cxx',
  'src/fcgi/istream_fcgi.cxx',
  include_directories: inc,
)
fcgi_client_dep = declare_dependency(
  link_with: fcgi_client,
  dependencies: [
    socket_dep,
    putil_dep,
    istream_dep,
    http_util_dep,
    stopwatch_dep,
  ],
)

libnfs = dependency('libnfs', required: get_option('nfs'))
if libnfs.found()
  nfs_client = static_library(
    'nfs_client',
    'src/nfs/Stock.cxx',
    'src/nfs/Client.cxx',
    'src/nfs/Error.cxx',
    'src/nfs/Cache.cxx',
    'src/nfs/Glue.cxx',
    include_directories: inc,
    dependencies: [
      libnfs,
    ],
  )
  nfs_client_dep = declare_dependency(
    compile_args: '-DHAVE_LIBNFS',
    link_with: nfs_client,
    dependencies: [
      event_dep,
    ],
  )
else
  nfs_client_dep = dependency('', required: false)
endif

delegate_client = static_library('delegate_client',
  'src/delegate/Client.cxx',
  'src/delegate/Glue.cxx',
  'src/delegate/HttpRequest.cxx',
  'src/delegate/Stock.cxx',
  include_directories: inc,
)
delegate_client_dep = declare_dependency(
  link_with: delegate_client,
  dependencies: [
    socket_dep,
    putil_dep,
    stock_dep,
  ],
)

session = static_library('session',
  'src/bp/session/Session.cxx',
  'src/bp/session/Lease.cxx',
  'src/bp/session/Id.cxx',
  'src/bp/session/Manager.cxx',
  'src/bp/session/Write.cxx',
  'src/bp/session/Read.cxx',
  'src/bp/session/Save.cxx',
  include_directories: inc,
)
session_dep = declare_dependency(link_with: session,
                                 dependencies: [event_dep,
                                                cookie_dep,
                                                raddress_dep])

widget = static_library('widget',
  'src/widget/Widget.cxx',
  'src/widget/Init.cxx',
  'src/widget/Root.cxx',
  'src/widget/Ptr.cxx',
  'src/widget/Cache.cxx',
  'src/widget/Class.cxx',
  'src/widget/Context.cxx',
  'src/widget/ForwardHeaders.cxx',
  'src/widget/CSession.cxx',
  'src/widget/Untrusted.cxx',
  'src/widget/View.cxx',
  'src/widget/Ref.cxx',
  'src/widget/Session.cxx',
  'src/widget/Uri.cxx',
  'src/widget/Approval.cxx',
  'src/widget/Error.cxx',
  'src/args.cxx',
  include_directories: inc,
)
widget_dep = declare_dependency(link_with: widget,
                                dependencies: [putil_dep, raddress_dep,
                                              translation_dep])

processor = static_library('processor',
  'src/parser/XmlParser.cxx',
  'src/parser/CssParser.cxx',
  'src/bp/WidgetContainerParser.cxx',
  'src/bp/WidgetLookupProcessor.cxx',
  'src/bp/XmlProcessor.cxx',
  'src/pheaders.cxx',
  'src/bp/CssProcessor.cxx',
  'src/bp/CssRewrite.cxx',
  'src/bp/TextProcessor.cxx',
  include_directories: inc,
)
processor_dep = declare_dependency(
  link_with: processor,
  dependencies: [
    istream_dep,
    putil_dep,
    stopwatch_dep,
  ],
)

control_server = static_library('control_server',
  'src/control/Server.cxx',
  'src/control/Handler.cxx',
  'src/control/Local.cxx',
  include_directories: inc,
)
control_server_dep = declare_dependency(
  link_with: control_server,
  dependencies: [
    event_net_dep,
  ],
)

sources = [
]

if uring_dep.found()
  sources += 'src/io/UringOpenStat.cxx'
endif

if nfs_client_dep.found()
  sources += [
    'src/nfs/RequestHandler.cxx',
    'src/nfs/Istream.cxx',
  ]
endif

executable(
  'cm4all-beng-proxy',
  sources,
  'src/delegate/RequestHandler.cxx',
  'src/capabilities.cxx',
  'src/address_string.cxx',
  'src/cluster/TcpBalancer.cxx',
  'src/bot.c',
  'libcommon/src/spawn/Server.cxx',
  'libcommon/src/spawn/Launch.cxx',
  'libcommon/src/spawn/Client.cxx',
  'libcommon/src/spawn/Glue.cxx',
  'libcommon/src/pg/Interval.cxx',
  'src/child_stock.cxx',
  'src/bp/CommandLine.cxx',
  'src/bp/Config.cxx',
  'src/bp/ConfigParser.cxx',
  'src/bp/Listener.cxx',
  'src/bp/Connection.cxx',
  'src/bp/RLogger.cxx',
  'src/bp/drop.cxx',
  'src/relocate_uri.cxx',
  'src/fcache.cxx',
  'src/bp/FileHeaders.cxx',
  'src/bp/FileHandler.cxx',
  'src/bp/EmulateModAuthEasy.cxx',
  'src/bp/AprMd5.cxx',
  'src/bp/ProxyHandler.cxx',
  'src/bp/Global.cxx',
  'src/bp/Handler.cxx',
  'src/bp/CoHandler.cxx',
  'src/bp/Auth.cxx',
  'src/bp/HttpAuth.cxx',
  'src/bp/TokenAuth.cxx',
  'src/load_file.cxx',
  'src/bp/file_not_found.cxx',
  'src/bp/file_enotdir.cxx',
  'src/bp/file_directory_index.cxx',
  'src/bp/ForwardHeaders.cxx',
  'src/hostname.cxx',
  'src/http_request.cxx',
  'src/http/ResponseHandler.cxx',
  'src/http/CoResponseHandler.cxx',
  'src/bp/Stats.cxx',
  'src/bp/Control.cxx',
  'src/PipeLease.cxx',
  'src/pipe_stock.cxx',
  'src/pipe_filter.cxx',
  'src/bp/ProxyWidget.cxx',
  'src/random.cxx',
  'src/bp/ForwardRequest.cxx',
  'src/bp/Request.cxx',
  'src/bp/RSession.cxx',
  'src/bp/ExternalSession.cxx',
  'src/bp/CollectCookies.cxx',
  'src/bp/CsrfProtection.cxx',
  'src/bp/CsrfToken.cxx',
  'src/bp/RError.cxx',
  'src/bp/Response.cxx',
  'src/bp/GenerateResponse.cxx',
  'src/resource_tag.cxx',
  'src/widget/RewriteUri.cxx',
  'src/file_request.cxx',
  'src/static_headers.cxx',
  'src/translation/SuffixRegistry.cxx',
  'src/translation/AddressSuffixRegistry.cxx',
  'src/tcp_stock.cxx',
  'src/istream_rubber.cxx',
  'src/sink_rubber.cxx',
  'src/lhttp_stock.cxx',
  'src/lhttp_request.cxx',
  'src/bp/errdoc.cxx',
  'src/RedirectHttps.cxx',
  'src/widget/FromRequest.cxx',
  'src/widget/FromSession.cxx',
  'src/widget/Registry.cxx',
  'src/widget/Resolver.cxx',
  'src/widget/Request.cxx',
  'src/widget/Inline.cxx',
  'src/istream_escape.cxx',
  'src/istream_html_escape.cxx',
  'src/ssl/SslSocketFilterFactory.cxx',
  'src/DirectResourceLoader.cxx',
  'src/CachedResourceLoader.cxx',
  'src/FilterResourceLoader.cxx',
  'src/BufferedResourceLoader.cxx',
  'src/widget/Frame.cxx',
  'src/istream_gb.cxx',
  'src/istream_unlock.cxx',
  'src/access_log/Client.cxx',
  'src/access_log/Launch.cxx',
  'src/access_log/Glue.cxx',
  'src/access_log/ConfigParser.cxx',
  'src/access_log/ChildErrorLog.cxx',
  'src/PInstance.cxx',
  'src/bp/Instance.cxx',
  'src/bp/Main.cxx',
  include_directories: inc,
  dependencies: [
    threads,
    event_uring_dep,
    avahi_dep,
    pool_dep,
    net_dep,
    raddress_dep,
    spawn_dep,
    http_server_dep,
    http_client_dep,
    http_cache_dep,
    ssl_dep,
    translation_dep,
    was_client_dep,
    stopwatch_dep,
    cgi_dep,
    fcgi_client_dep,
    session_dep,
    widget_dep,
    processor_dep,
    nfs_client_dep,
    thread_pool_dep,
    delegate_client_dep,
    control_server_dep,
    nghttp2_dep,
    cluster_dep,
    sodium_dep,
    libcrypt,
    libcxx,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-beng-lb',
  'src/capabilities.cxx',
  'src/tcp_stock.cxx',
  'src/pipe_stock.cxx',
  'src/address_string.cxx',
  'src/cluster/AddressList.cxx',
  'src/cluster/ConnectBalancer.cxx',
  'src/cluster/TcpBalancer.cxx',
  'src/http/Address.cxx',
  'src/http/ResponseHandler.cxx',
  'src/istream_gb.cxx',
  'src/access_log/Client.cxx',
  'src/access_log/Launch.cxx',
  'src/access_log/Glue.cxx',
  'src/access_log/ConfigParser.cxx',
  'src/RedirectHttps.cxx',
  'src/PInstance.cxx',
  'src/lb/Listener.cxx',
  'src/lb/SimpleHttpResponse.cxx',
  'src/lb/HttpConnection.cxx',
  'src/lb/RLogger.cxx',
  'src/lb/ResolveConnect.cxx',
  'src/lb/LuaHttpRequestHandler.cxx',
  'src/lb/TranslationHttpRequestHandler.cxx',
  'src/lb/TcpConnection.cxx',
  'src/lb/ForwardHttpRequest.cxx',
  'src/lb/LuaHandler.cxx',
  'src/lb/LuaInitHook.cxx',
  'src/lb/LuaGoto.cxx',
  'src/lb/Stats.cxx',
  'src/lb/Control.cxx',
  'src/lb/JvmRoute.cxx',
  'src/lb/Headers.cxx',
  'src/lb/Session.cxx',
  'src/lb/Check.cxx',
  'src/lb/Cookie.cxx',
  'src/lb/CommandLine.cxx',
  'src/lb/Config.cxx',
  'src/lb/ConfigParser.cxx',
  'src/lb_check.cxx',
  'src/lb/Setup.cxx',
  'src/lb/GotoMap.cxx',
  'src/lb/Branch.cxx',
  'src/lb/MemberHash.cxx',
  'src/lb/Cluster.cxx',
  'src/lb/ClusterConfig.cxx',
  'src/lb/TranslationHandler.cxx',
  'src/lb/TranslationCache.cxx',
  'src/lb/GotoConfig.cxx',
  'src/lb/MonitorController.cxx',
  'src/lb/MonitorRef.cxx',
  'src/lb/MonitorStock.cxx',
  'src/lb/MonitorManager.cxx',
  'src/lb/PingMonitor.cxx',
  'src/lb/SynMonitor.cxx',
  'src/lb/ExpectMonitor.cxx',
  'src/lb/Instance.cxx',
  'src/lb/Main.cxx',

  # the following sources are only necessary for the translation client
  'src/widget/View.cxx',

  include_directories: inc,
  dependencies: [
    threads,
    liblua,
    libpcre,
    sodium_dep,
    avahi_dep,
    pool_dep,
    io_dep,
    socket_dep,
    event_dep,
    net_dep,
    ssl_dep,
    pg_dep,
    nghttp2_dep,
    http_server_dep,
    http_client_dep,
    putil_dep,
    eutil_dep,
    stock_dep,
    translation_dep,
    spawn_dep,
    expand_dep,
    stopwatch_dep,
    cookie_dep,
    control_server_dep,
    lua_dep,
    thread_pool_dep,
    pcre_dep,
    cluster_dep,
    libcxx,
  ],
  install: true,
  install_dir: 'sbin',
)

if get_option('certdb')
  executable(
    'cm4all-certdb',
    'src/lb/Config.cxx',
    'src/lb/ConfigParser.cxx',
    'src/lb/GotoConfig.cxx',
    'src/lb/ClusterConfig.cxx',
    'src/certdb/Progress.cxx',
    'src/certdb/WrapKey.cxx',
    'src/certdb/CertDatabase.cxx',
    'src/certdb/Migrate.cxx',
    'src/certdb/FromResult.cxx',
    'src/certdb/Wildcard.cxx',
    'src/certdb/GlueHttpClient.cxx',
    'src/certdb/JWS.cxx',
    'src/certdb/AcmeError.cxx',
    'src/certdb/AcmeHttp.cxx',
    'src/certdb/AcmeDns.cxx',
    'src/certdb/AcmeClient.cxx',
    'src/certdb/AcmeChallenge.cxx',
    'src/certdb/AcmeAccount.cxx',
    'src/certdb/AcmeAuthorization.cxx',
    'src/certdb/AcmeKey.cxx',
    'src/certdb/FakeAcmeClient.cxx',
    'src/certdb/AcmeMain.cxx',
    'src/certdb/Main.cxx',
    'src/access_log/ConfigParser.cxx',
    include_directories: inc,
    dependencies: [
      boost_json_dep,
      pcre_dep,
      avahi_dep,
      sodium_dep,
      ssl_dep,
      pg_dep,
      curl_dep,
      net_dep,
      http_dep,
      io_dep,
      jwt_dep,
      libcxx,
    ],
    install: true,
  )
endif

executable(
  'cm4all-beng-proxy-log-cat',
  'src/access_log/Server.cxx',
  'src/access_log/Cat.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    http_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-json',
  'src/access_log/Server.cxx',
  'src/access_log/Json.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    http_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-lua',
  'src/access_log/Server.cxx',
  'src/access_log/Lua.cxx',
  'src/access_log/Launch.cxx',
  include_directories: inc,
  dependencies: [
    lua_dep,
    spawn_dep,
    net_dep,
    http_dep,
    liblua,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-traffic',
  'src/access_log/Server.cxx',
  'src/access_log/Traffic.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    http_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-split',
  'src/access_log/Server.cxx',
  'src/access_log/Split.cxx',
  include_directories: inc,
  dependencies: [
    system_dep,
    net_dep,
    http_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-forward',
  'src/access_log/Forward.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    http_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-exec',
  'src/access_log/Exec.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'cm4all-beng-proxy-log-tee',
  'src/access_log/Launch.cxx',
  'src/access_log/Tee.cxx',
  include_directories: inc,
  dependencies: [
    spawn_dep,
    system_dep,
    net_dep,
    libcxx,
  ],
  install: true,
)

executable(
  'delegate-helper',
  'src/delegate/Helper.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    util_dep,
    libcxx,
  ],
  install: true,
  install_dir: 'lib/cm4all/beng-proxy/delegate/bin',
)

executable(
  'cm4all-beng-control',
  'src/control/Main.cxx',
  'src/control/Client.cxx',
  include_directories: inc,
  dependencies: [
    net_dep,
    io_dep,
    util_dep,
    libcxx,
  ],
  install: true,
)

install_headers(
  'include/beng-proxy/Control.hxx',
  'include/beng-proxy/Headers.hxx',
  subdir: 'beng-proxy',
)

subdir('doc')
subdir('test')
subdir('libcommon/test/util')
subdir('libcommon/test/uri')
subdir('libcommon/test/http')
subdir('libcommon/test/io')
if get_option('certdb')
  subdir('libcommon/test/pg')
endif
subdir('libcommon/test/sodium')
