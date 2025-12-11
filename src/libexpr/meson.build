project(
  'nix-expr',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    # TODO(Qyriad): increase the warning level
    'warning_level=1',
    'errorlogs=true', # Please print logs for tests that fail
  ],
  meson_version : '>= 1.1',
  license : 'LGPL-2.1-or-later',
)

cxx = meson.get_compiler('cpp')

subdir('nix-meson-build-support/deps-lists')

configdata_pub = configuration_data()
configdata_priv = configuration_data()

deps_private_maybe_subproject = []
deps_public_maybe_subproject = [
  dependency('nix-util'),
  dependency('nix-store'),
  dependency('nix-fetchers'),
]
subdir('nix-meson-build-support/subprojects')
subdir('nix-meson-build-support/big-objs')

# Check for each of these functions, and create a define like `#define HAVE_LCHOWN 1`.
check_funcs = [
  'sysconf',
]
foreach funcspec : check_funcs
  define_name = 'HAVE_' + funcspec.underscorify().to_upper()
  define_value = cxx.has_function(funcspec).to_int()
  configdata_priv.set(define_name, define_value)
endforeach

boost = dependency(
  'boost',
  modules : [
    'container',
    'context',
    'thread',
  ],
  include_type : 'system',
)
# boost is a public dependency, but not a pkg-config dependency unfortunately, so we
# put in `deps_other`.
deps_other += boost

nlohmann_json = dependency('nlohmann_json', version : '>= 3.9')
deps_public += nlohmann_json

bdw_gc_required = get_option('gc').disable_if(
  'address' in get_option('b_sanitize'),
  error_message : 'Building with Boehm GC and ASAN is not supported',
)

bdw_gc = dependency('bdw-gc', required : bdw_gc_required)
if bdw_gc.found()
  deps_public += bdw_gc
  foreach funcspec : [
    'pthread_getattr_np',
  ]
    define_name = 'HAVE_' + funcspec.underscorify().to_upper()
    define_value = cxx.has_function(funcspec).to_int()
    configdata_priv.set(define_name, define_value)
  endforeach
  if host_machine.system() == 'cygwin'
    # undefined reference to `__wrap__Znwm'
    configdata_pub.set('GC_NO_INLINE_STD_NEW', 1)
  endif
endif
# Used in public header. Affects ABI!
configdata_pub.set('NIX_USE_BOEHMGC', bdw_gc.found().to_int())

# GHC GC support (experimental)
# Note: GHC GC and Boehm GC are mutually exclusive
ghc_gc_enabled = get_option('ghc_gc').enabled()
if ghc_gc_enabled and bdw_gc.found()
  error('Cannot enable both Boehm GC and GHC GC. Use -Dgc=disabled to disable Boehm GC.')
endif

# Used in public header. Affects ABI!
configdata_pub.set('NIX_USE_GHC_GC', ghc_gc_enabled.to_int())

# GHC runtime dependency (when GHC GC is enabled)
# The actual include_dirs setup happens after subdir('include/nix/expr')
ghc_rts_sources = []
ghc_rts_include_dir = ''
ghc_rts_lib_dir = ''
ghc_version = ''
if ghc_gc_enabled
  # Find GHC and ghc-pkg
  ghc = find_program('ghc', required : true)
  ghc_pkg = find_program('ghc-pkg', required : true)

  # Get GHC library directory
  ghc_libdir = run_command(ghc, '--print-libdir', check : true).stdout().strip()
  ghc_version = run_command(ghc, '--numeric-version', check : true).stdout().strip()

  # Get RTS include directories from ghc-pkg
  # Output format: "include-dirs: /path1\n              /path2\n              /path3"
  ghc_rts_include_output = run_command(ghc_pkg, 'field', 'rts', 'include-dirs', check : true).stdout().strip()
  # Extract just the RTS include directory (the one containing HsFFI.h)
  ghc_rts_include_lines = ghc_rts_include_output.split('\n')
  foreach line : ghc_rts_include_lines
    stripped = line.strip()
    if stripped.startswith('include-dirs:')
      stripped = stripped.replace('include-dirs:', '').strip()
    endif
    if stripped.contains('rts-')
      ghc_rts_include_dir = stripped
      break
    endif
  endforeach

  if ghc_rts_include_dir == ''
    error('Could not find GHC RTS include directory')
  endif

  message('GHC RTS include directory: ' + ghc_rts_include_dir)

  # Get RTS library directories from ghc-pkg
  ghc_rts_libdir_output = run_command(ghc_pkg, 'field', 'rts', 'library-dirs', check : true).stdout().strip()
  ghc_rts_libdir_lines = ghc_rts_libdir_output.split('\n')
  foreach line : ghc_rts_libdir_lines
    stripped = line.strip()
    if stripped.startswith('library-dirs:')
      stripped = stripped.replace('library-dirs:', '').strip()
    endif
    if stripped.contains('rts-')
      ghc_rts_lib_dir = stripped
      break
    endif
  endforeach

  if ghc_rts_lib_dir == ''
    error('Could not find GHC RTS library directory')
  endif

  message('GHC RTS library directory: ' + ghc_rts_lib_dir)
  message('GHC GC enabled, using GHC ' + ghc_version + ' at: ' + ghc_libdir)
endif

toml11 = dependency(
  'toml11',
  version : '>=3.7.0',
  method : 'cmake',
  include_type : 'system',
)

configdata_priv.set(
  'HAVE_TOML11_4',
  toml11.version().version_compare('>= 4.0.0').to_int(),
)

deps_other += toml11

config_priv_h = configure_file(
  configuration : configdata_priv,
  output : 'expr-config-private.hh',
)

subdir('nix-meson-build-support/common')
subdir('nix-meson-build-support/asan-options')

parser_tab = custom_target(
  input : 'parser.y',
  output : [
    'parser-tab.cc',
    'parser-tab.hh',
  ],
  command : [
    'bison',
    '-v',
    '-o',
    '@OUTPUT0@',
    '@INPUT@',
    '-d',
  ],
  # NOTE(Qyriad): Meson doesn't support installing only part of a custom target, so we add
  # an install script below which removes parser-tab.cc.
  install : true,
  install_dir : get_option('includedir') / 'nix',
)

lexer_tab = custom_target(
  input : [
    'lexer.l',
    parser_tab,
  ],
  output : [
    'lexer-tab.cc',
    'lexer-tab.hh',
  ],
  command : [
    'flex',
    '-Cf', # Use full scanner tables
    '--outfile', '@OUTPUT0@',
    '--header-file=' + '@OUTPUT1@',
    '@INPUT0@',
  ],
  # NOTE(Qyriad): Meson doesn't support installing only part of a custom target, so we add
  # an install script below which removes lexer-tab.cc.
  install : true,
  install_dir : get_option('includedir') / 'nix',
)

subdir('nix-meson-build-support/generate-header')

generated_headers = []
foreach header : [
  'imported-drv-to-derivation.nix',
  'fetchurl.nix',
]
  generated_headers += gen_header.process(header)
endforeach

sources = files(
  'attr-path.cc',
  'attr-set.cc',
  'eval-cache.cc',
  'eval-error.cc',
  'eval-gc.cc',
  'eval-profiler-settings.cc',
  'eval-profiler.cc',
  'eval-settings.cc',
  'eval.cc',
  'function-trace.cc',
  'get-drvs.cc',
  'json-to-value.cc',
  'lexer-helpers.cc',
  'nixexpr.cc',
  'parallel-eval.cc',
  'paths.cc',
  'primops.cc',
  'print-ambiguous.cc',
  'print.cc',
  'search-path.cc',
  'symbol-table.cc',
  'value-to-json.cc',
  'value-to-xml.cc',
  'value.cc',
  'value/context.cc',
)

subdir('include/nix/expr')

# Complete GHC GC setup (include_dirs is now defined)
if ghc_gc_enabled
  # Note: GHC RTS include directories are no longer needed (Iteration 19: Lazy Loading)
  # We use dlopen/dlsym instead of direct header includes.
  # ghc_include_dirs = include_directories(ghc_rts_include_dir)
  # include_dirs += ghc_include_dirs

  # GHC RTS library - threaded version for multi-threading support
  # Get RTS version from ghc-pkg
  ghc_rts_version_output = run_command(ghc_pkg, 'field', 'rts', 'version', check : true).stdout().strip()
  ghc_rts_version = ghc_rts_version_output.replace('version:', '').strip()
  message('GHC RTS version: ' + ghc_rts_version)

  # The dynamic RTS library is in the parent directory of the rts lib dir
  # e.g., libHSrts-1.0.2_thr-ghc9.8.4.so in x86_64-linux-ghc-9.8.4/
  ghc_dyn_lib_dir = ghc_libdir + '/../lib/x86_64-linux-ghc-' + ghc_version

  # Try dynamic library first (required for linking with shared libraries like libnixexpr.so)
  # Dynamic library naming: libHSrts-<version>_thr-ghc<ghc_version>.so
  ghc_rts = cxx.find_library('HSrts-' + ghc_rts_version + '_thr-ghc' + ghc_version,
    dirs : [ghc_dyn_lib_dir],
    required : false,
    static : false,
  )

  if not ghc_rts.found()
    # Try the static library as fallback (works for static executables)
    ghc_rts = cxx.find_library('HSrts-' + ghc_rts_version + '_thr',
      dirs : [ghc_rts_lib_dir],
      required : false,
      static : true,
    )
  endif

  if not ghc_rts.found()
    ghc_rts = cxx.find_library('HSrts_thr',
      dirs : [ghc_rts_lib_dir],
      required : false,
      static : true,
    )
  endif

  if not ghc_rts.found()
    # Try non-threaded as fallback
    ghc_rts = cxx.find_library('HSrts-' + ghc_rts_version,
      dirs : [ghc_rts_lib_dir],
      required : false,
      static : true,
    )
  endif

  if not ghc_rts.found()
    # List available libraries for debugging
    message('Available libraries in ' + ghc_rts_lib_dir + ':')
    run_command('ls', '-la', ghc_rts_lib_dir, check : false)
    message('Available libraries in ' + ghc_dyn_lib_dir + ':')
    run_command('ls', '-la', ghc_dyn_lib_dir, check : false)
    error('Could not find GHC RTS library')
  endif

  # Note: GHC RTS is NOT linked at build time (Iteration 19: Lazy Loading)
  # It will be loaded on demand via dlopen() when initGHCRuntime() is called.
  # deps_other += ghc_rts  # Commented out for lazy loading

  # Add GHC GC source file
  ghc_rts_sources = files('ghc-gc.cc')

  # Phase 2: Haskell compilation removed - using pure C++ GC implementation
  # Compile Haskell allocator module
  # This generates GhcAlloc.o and GhcAlloc_stub.h
  # ghc_alloc_hs = files('GhcAlloc.hs')

  # Get GHC library directory for package database
  # ghc_pkg_db = ghc_libdir / 'package.conf.d'

  # Custom target to compile Haskell code
  # -c: compile only, don't link
  # -dynamic: generate position-independent code for shared library
  # -fPIC: position independent code
  # -fexternal-dynamic-refs: use external references for dynamic symbols (required for PIC)
  # -O2: optimization level
  # -package containers: needed for Data.IntMap (Iteration 23)
  # ghc_alloc_obj = custom_target(
  #   'GhcAlloc.o',
  #   input : ghc_alloc_hs,
  #   output : ['GhcAlloc.o', 'GhcAlloc_stub.h'],
  #   command : [
  #     ghc,
  #     '-c',
  #     '-dynamic',
  #     '-fPIC',
  #     '-fexternal-dynamic-refs',
  #     '-O2',
  #     '-package-db', ghc_pkg_db,
  #     '-package', 'containers',  # Iteration 23: For Data.IntMap
  #     '-outputdir', '@OUTDIR@',
  #     '-o', '@OUTPUT0@',
  #     '-stubdir', '@OUTDIR@',
  #     '@INPUT@',
  #   ],
  #   build_by_default : true,
  # )

  # Create a shared library from the Haskell object using GHC as the linker.
  # This is necessary because GHC generates non-PIC relocations for internal
  # data structures (closures, info tables) that can only be properly relocated
  # when GHC controls the linking process.
  # We link the threaded RTS directly to make the library self-contained.
  # ghc_alloc_lib = custom_target(
  #   'libghcalloc.so',
  #   input : ghc_alloc_obj[0],  # GhcAlloc.o
  #   output : 'libghcalloc.so',
  #   command : [
  #     ghc,
  #     '-shared',
  #     '-dynamic',
  #     '-fPIC',
  #     '-threaded',  # Link threaded RTS
  #     '-package', 'base',
  #     '-package', 'containers',  # Iteration 23: For Data.IntMap
  #     '-L' + ghc_dyn_lib_dir,
  #     '-lHSrts-' + ghc_rts_version + '_thr-ghc' + ghc_version,
  #     '-o', '@OUTPUT@',
  #     '@INPUT@',
  #   ],
  #   build_by_default : true,
  #   install : true,
  #   install_dir : get_option('libdir'),
  # )

  # ===========================================================================
  # Iteration 19: Lazy Loading of GHC Libraries
  # ===========================================================================
  # We no longer link GHC libraries (base, ghc-prim, ghc-bignum, rts) at build
  # time. Instead, they are loaded on demand via dlopen() when initGHCRuntime()
  # is called.
  #
  # This eliminates the ~18MB of GHC library loading overhead at startup,
  # which was causing an 8x slowdown even though allocations use pure C calloc.
  #
  # The libghcalloc.so library is still built and installed, but it's loaded
  # lazily rather than linked directly.
  # ===========================================================================

  # Iteration 64: Build libghcalloc.so directly with meson
  # This creates the Haskell FFI library that provides GHC-managed allocation

  # Get RTS library path for linking
  ghc_rts_lib_path = ghc_libdir / '..' / 'lib' / 'x86_64-linux-ghc-' + ghc_version

  # Step 1: Compile NixAlloc.hs to object file
  nixalloc_o = custom_target(
    'NixAlloc.o',
    output: ['NixAlloc.o', 'NixAlloc.hi', 'NixAlloc_stub.h'],
    input: files('ghc-alloc/NixAlloc.hs'),
    command: [
      ghc, '-c',
      '-O2',
      '-threaded',
      '-fPIC',
      '-dynamic',
      '-outputdir', '@OUTDIR@',
      '-o', '@OUTPUT0@',
      '@INPUT@'
    ],
    build_by_default: true,
  )

  # Step 2: Link into shared library
  ghcalloc_lib = custom_target(
    'libghcalloc',
    output: 'libghcalloc.so',
    input: nixalloc_o,
    command: [
      ghc, '-shared',
      '-dynamic',
      '-threaded',
      '-rtsopts',
      '-o', '@OUTPUT@',
      '@INPUT0@',
      '-fPIC',
      '-L' + ghc_rts_lib_path,
      '-lHSrts-1.0.2_thr-ghc' + ghc_version
    ],
    build_by_default: true,
    install: true,
    install_dir: get_option('libdir'),
  )

  # Note: We need libdl for dlopen/dlsym
  libdl = cxx.find_library('dl', required : true)
  deps_other += libdl

  # Store the GHC library path for runtime lookup (if needed)
  ghc_hs_lib_dir = ghc_libdir / 'x86_64-linux-ghc-' + ghc_version
  message('GHC libraries available at: ' + ghc_hs_lib_dir + ' (loaded lazily via dlopen)')
endif

subdir('primops')

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/windows-version')

# Collect all source files for the library
all_sources = [sources, ghc_rts_sources, config_priv_h, parser_tab, lexer_tab, generated_headers]

# NOTE: The GhcAlloc.o Haskell object file is NOT linked into libnixexpr.so
# because GHC generates non-PIC relocations for internal data structures
# (closures, info tables) that can't be used in a shared library.
# The Haskell code compiles correctly (verified in Iteration 12), but
# linking requires creating a separate shared library with GHC as linker.
# For now, C stubs in ghc-gc.cc provide identical FFI implementations.

this_library = library(
  'nixexpr',
  all_sources,
  soversion : nix_soversion,
  dependencies : deps_public + deps_private + deps_other,
  include_directories : include_dirs,
  link_args : linker_export_flags,
  prelink : true, # For C++ static initializers
  install : true,
  cpp_pch : do_pch ? [ 'pch/precompiled-headers.hh' ] : [],
)

# Iteration 64: Ensure libghcalloc.so is built before libnixexpr.so
# This makes the library available for runtime loading via dlopen()
if ghc_gc_enabled
  # Note: We don't add ghcalloc_lib as a link dependency because it's loaded
  # dynamically at runtime via dlopen(). We just ensure it's built.
  meson.add_install_script('sh', '-c',
    'echo "libghcalloc.so installed to: ' + get_option('prefix') / get_option('libdir') + '"')
endif

install_headers(headers, subdir : 'nix/expr', preserve_path : true)

libraries_private = []

subdir('nix-meson-build-support/export')
