# Shim for CMake 4's removed SetSystemProcessor module.
#
# Vendored soxr 0.1.3 does `include(SetSystemProcessor)` and calls
# `set_system_processor()`. CMake 4.4 removed that module, so configuration
# fails outright.
#
# The original macro set CMAKE_SYSTEM_PROCESSOR plus a family of
# per-architecture variables. soxr consults only CMAKE_SYSTEM_PROCESSOR, in a
# single place: the ARM SIMD test at CMakeLists.txt:137, which never fires on
# x86. So filling in that one variable is enough, and CMake 4 already computes
# it correctly on every platform we build for.
#
# This lives outside the vendored tree on purpose. Editing soxr's own files would
# mean the next upstream release arrives as a conflict.

macro(set_system_processor)
  if(NOT CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _neiro_sysproc)
    set(CMAKE_SYSTEM_PROCESSOR "${_neiro_sysproc}")
  endif()
endmacro()
