project('gstspoutsrc', 'cpp',
  version: '1.0.0',
  default_options: [
    'warning_level=2',
    'cpp_std=c++20'
  ]
)

# 1) Options
spout_sdk_path = get_option('spout_sdk_path')  # e.g. "C:/SPOUT2SDK"
pluginsdir     = get_option('pluginsdir')      # e.g. "C:/gstreamer/1.0/msvc_x86_64/lib/gstreamer-1.0"

# 2) GStreamer dependencies
gst_dep       = dependency('gstreamer-1.0', required: true)
gst_base_dep  = dependency('gstreamer-base-1.0', required: true)
gst_video_dep = dependency('gstreamer-video-1.0', required: true)
glib_dep      = dependency('glib-2.0', required: true)
gst_d3d11_dep = dependency('gstreamer-d3d11-1.0', required: true)

# 3) Include path for Spout headers
# We need to add all potential locations where SpoutDX.h might be found
inc_spout_root = include_directories(spout_sdk_path)
inc_spout_include = include_directories(join_paths(spout_sdk_path, 'include'))
inc_spout_dx = include_directories(join_paths(spout_sdk_path, 'include', 'SpoutDX'))
inc_spout_dx12 = include_directories(join_paths(spout_sdk_path, 'include', 'SpoutDX12'))

# 4) Link to the dynamic library for spoutDX12 (MD version).
#    The .lib is at C:/SPOUT2SDK/MD/lib/SpoutDX12.lib
#    We'll also need the matching SpoutDX12.dll at runtime
#    (e.g. copy it into the same folder as gstspoutsrc.dll).
spoutdx12_lib_path = join_paths(spout_sdk_path, 'MD', 'lib', 'SpoutDX12.lib')

spoutdx12_dep = declare_dependency(
  include_directories: [inc_spout_root, inc_spout_include, inc_spout_dx, inc_spout_dx12],
  link_args: [
    # /LIBPATH not strictly needed if we give an absolute path.
    spoutdx12_lib_path
  ]
)

# 5) Our plugin source files
sources = [
  'gstspoutsrc.cpp',
  'gstspoutsrc.h',
]

# 6) Build as a shared library that GStreamer can load.
gstspoutsrc_lib = shared_library(
  'gstspoutsrc',  # produces gstspoutsrc.dll
  sources,
  dependencies: [
    gst_dep,
    gst_base_dep,
    gst_video_dep,
    glib_dep,
    gst_d3d11_dep,  # <-- CRITICAL: Adding the D3D11 dependency
    spoutdx12_dep,  # <-- link the spoutDX12 dependency
  ],
  install: true,
  install_dir: pluginsdir
)

message('Building gstspoutsrc with spout SDK at ' + spout_sdk_path)