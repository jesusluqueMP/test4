:: Point this to where GStreamer is installed:
set GSTREAMER_1_0_ROOT_X86_64=C:\gstreamer\1.0\msvc_x86_64

:: Add GStreamer bin to PATH so the compiler/linker can find .dll files:
set PATH=%GSTREAMER_1_0_ROOT_X86_64%\bin;%PATH%

:: Tell pkg-config where to find the .pc files for GStreamer:
set PKG_CONFIG_PATH=%GSTREAMER_1_0_ROOT_X86_64%\lib\pkgconfig

:: Confirm pkg-config can find gstreamer:
pkg-config --modversion gstreamer-1.0