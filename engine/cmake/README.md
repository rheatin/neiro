# Shims for vendored dependencies.
#
# soxr 0.1.3 predates CMake 4 and includes two modules that CMake 4 removed.
# Rather than edit the vendored source — which would turn every future upgrade
# into a merge conflict — provide the missing pieces here and put this directory
# ahead of soxr's own include path.
#
# Only what is actually needed is implemented. soxr calls
# `set_system_processor()` for exactly one test, an ARM SIMD branch that is
# inactive on x86, so setting the standard variables is sufficient.

# Stand-in for CMake's removed SetSystemProcessor module.
#
# The original set CMAKE_SYSTEM_PROCESSOR and a family of per-architecture
# variables. Only CMAKE_SYSTEM_PROCESSOR is consulted by soxr (line ~137, the
# ARM SIMD branch), and CMake 4 sets it correctly on its own.
macro(set_system_processor)
  if(NOT CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _neiro_proc)
    set(CMAKE_SYSTEM_PROCESSOR "${_neiro_proc}")
  endif()
endmacro()
