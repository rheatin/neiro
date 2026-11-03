# Select the C runtime for MinGW before CMake compiles anything.
#
# Why this is a toolchain file and not something in CMakeLists.txt:
# `cmake_minimum_required()` implicitly runs `project()`, and `project()` is
# what compiles the first test program. Any flag set in CMakeLists.txt --
# add_compile_options, set(CMAKE_CXX_FLAGS ...), anything -- is applied after
# that check has already failed. Only the _FLAGS_INIT variables and the compiler
# selection itself are read early enough, and a toolchain file is the documented
# place to set them.
#
# What it fixes. Measured 2026-11-02 on MinGW-Builds 16.2.0-rt_v14-rev1: that
# toolchain ships libucrt.a and no libmsvcrt.a at all, so its default spec
# cannot link a hello world -- `ld: cannot find -lmsvcrt`. The same toolchain's
# libwinpthread.a is a UCRT build whose thread object references
# `__intrinsic_setjmpex`, which libmsvcrt.a does not define, so folding
# winpthread into nae.dll failed with `undefined reference to
# __intrinsic_setjmpex` on a repository that had not changed.
#
# The probe runs the compiler the user is about to use. CMAKE_CXX_COMPILER is
# usually not set yet at this point, so fall back to $CXX and then to the PATH
# search CMake itself would perform.
#
# On a toolchain that has no libucrt.a (msvcrt-only MinGW, clang, GCC on Linux
# and macOS) nothing is set and the default stays the default.

if(CMAKE_CXX_COMPILER)
  set(_neiro_probe "${CMAKE_CXX_COMPILER}")
elseif(DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
  set(_neiro_probe "$ENV{CXX}")
else()
  # Must not be preceded by set() to an empty value: find_program treats an
  # existing normal variable as "already found" and skips the search entirely.
  find_program(_neiro_probe NAMES c++ g++)
endif()

if(_neiro_probe)
  execute_process(
    COMMAND "${_neiro_probe}" -print-file-name=libucrt.a
    OUTPUT_VARIABLE _neiro_ucrt_lib
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_neiro_ucrt_lib AND EXISTS "${_neiro_ucrt_lib}")
    # Directory scope, not per target: the harnesses link the DLL into the same
    # process, and passing this to only one side is how two halves of one
    # process end up on different runtimes.
    set(CMAKE_C_FLAGS_INIT "-mcrtdll=ucrt")
    set(CMAKE_CXX_FLAGS_INIT "-mcrtdll=ucrt")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcrtdll=ucrt")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-mcrtdll=ucrt")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-mcrtdll=ucrt")
  endif()
endif()
