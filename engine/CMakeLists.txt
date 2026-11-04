cmake_minimum_required(VERSION 3.20)

# The C runtime is chosen in cmake/mingw-ucrt.cmake, not here.
#
# It cannot be done in this file. `cmake_minimum_required()` implicitly runs
# `project()`, and `project()` compiles the first test program; every line below
# that point is too late. On the UCRT-only MinGW builds (16.2.0-rt_v14 ships
# libucrt.a and no libmsvcrt.a) that first check fails with
# `ld: cannot find -lmsvcrt` before anything here is read.
#
# Configure with `cmake --preset windows`, or pass
# `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-ucrt.cmake`.

project(neiro_audio_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# soxr 0.1.3 declares cmake_minimum_required(VERSION 3.1). CMake 4 removed
# compatibility with anything below 3.5 and errors out rather than warning, so
# the vendored project cannot configure as-is.
#
# CMAKE_POLICY_VERSION_MINIMUM is the supported override for exactly this
# situation, and it must be a cache variable set before add_subdirectory — the
# guard reads it, not the calling scope's policy stack. It applies to the whole
# project, which is acceptable: we have no policies between 3.1 and 3.5 in use.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# soxr — sample rate conversion.
#
# Vendored under third_party/ rather than fetched at build time so the build is
# self-contained: no network, reproducible in CI, and the exact version we
# tested is the one that ships. LGPL, which keeps the project permissive.
#
# Its CMakeLists predates CMake 4, hence the policy pin. Do not "fix" it
# upstream here; the flag is the smaller change.
# ---------------------------------------------------------------------------
set(NEIRO_SOXR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr)

# soxr's own option names, not SOXR_-prefixed ones — the guards at its
# CMakeLists.txt:282-290 test BUILD_TESTS / BUILD_EXAMPLES / BUILD_LSR_TESTS
# directly, and a mismatched name silently leaves them ON. Their test
# subdirectories are not vendored, so a stray BUILD_TESTS=ON fails configure.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LSR_TESTS OFF CACHE BOOL "" FORCE)
# soxr is linked statically into nae.dll, so nothing should pull in a .dll.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# OpenMP off. soxr defaults it on, which is wrong for us on two counts: it drags
# in a -fopenmp runtime dependency, and a resampler that decides its own thread
# count is a resampler that can oversubscribe the machine mid-render. The audio
# thread is already the one place we want to be in control of scheduling.
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
# No EXCLUDE_FROM_ALL here: the soxr target has to actually build, and building
# it costs about a second. EXCLUDE_FROM_ALL would leave a target that is linked
# but never compiled, which fails at link time with a bare "cannot find -lsoxr".
add_subdirectory(${NEIRO_SOXR_DIR} ${CMAKE_CURRENT_BINARY_DIR}/soxr)

# ---------------------------------------------------------------------------
# The engine DLL.
#
# Exported with a C ABI on purpose: the Tauri/Rust host reaches it through
# bindgen, and a C ABI keeps that boundary stable no matter what the C++ side
# does internally. Export only the NAE_* surface, never the COM plumbing.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# FFmpeg — audio file decoding.
#
# Found via the scoop ffmpeg-shared package rather than vendored, for now. That
# build is GPL, so linking it makes nae.dll GPL: fine for proving the decode path
# works, NOT fine to ship. Before release this must become an LGPL build (they
# differ only by which optional components are compiled in, and this player
# needs none of the GPL ones — no x264, no xvid, nothing). It is a link-time
# change, so nothing above it has to move.
#
# Point NEIRO_FFMPEG_ROOT at another tree to switch builds.
# ---------------------------------------------------------------------------
if(WIN32)
  set(NEIRO_FFMPEG_ROOT "$ENV{USERPROFILE}/scoop/apps/ffmpeg-shared/current"
      CACHE PATH "Root of an FFmpeg shared build (with include/ and lib/)")
else()
  # Homebrew: /opt/homebrew on Apple Silicon, /usr/local on Intel. A caller can
  # override with -DNEIRO_FFMPEG_ROOT=<prefix>.
  find_program(NEIRO_BREW_BIN brew)
  if(NEIRO_BREW_BIN)
    execute_process(COMMAND ${NEIRO_BREW_BIN} --prefix
                    OUTPUT_VARIABLE NEIRO_BREW_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(NOT NEIRO_BREW_PREFIX)
    set(NEIRO_BREW_PREFIX "/opt/homebrew")
  endif()
  set(NEIRO_FFMPEG_ROOT "${NEIRO_BREW_PREFIX}"
      CACHE PATH "Root of an FFmpeg install (with include/ and lib/)")
endif()

if(EXISTS "${NEIRO_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  set(NEIRO_HAVE_FFMPEG ON)
  message(STATUS "FFmpeg headers found in ${NEIRO_FFMPEG_ROOT}")

else()
  set(NEIRO_HAVE_FFMPEG OFF)
  message(WARNING
    "FFmpeg not found at ${NEIRO_FFMPEG_ROOT}. File decoding will be disabled; "
    "the engine still builds and the test tone still works.")
endif()

# Platform-neutral audio core. This target must not include a platform header:
# policy/resolver/diagnostics, Hilbert, routing maths, dither, resampler and the
# FFmpeg decoder all live here. The backend (WASAPI today) links it; the same
# target can be compiled by a macOS/Android toolchain once its backend exists.
add_library(neiro_core STATIC
  policy.cpp
  hilbert.cpp
  dither.cpp
  resampler.cpp
  routing.cpp
  clip_guard.cpp
  partitioned_convolver.cpp
  loudness.cpp
  linear_phase_eq.cpp
)
target_include_directories(neiro_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${NEIRO_SOXR_DIR}/src)
target_link_libraries(neiro_core PUBLIC soxr)
set_target_properties(neiro_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NEIRO_HAVE_FFMPEG)
  target_sources(neiro_core PRIVATE decoder.cpp)
  target_include_directories(neiro_core PUBLIC "${NEIRO_FFMPEG_ROOT}/include")
  target_link_directories(neiro_core PUBLIC "${NEIRO_FFMPEG_ROOT}/lib")
  target_compile_definitions(neiro_core PUBLIC NEIRO_HAVE_FFMPEG=1)
  target_link_libraries(neiro_core PUBLIC avformat avcodec swresample avutil)
  if(APPLE)
    # Homebrew FFmpeg is normally linked as dylibs under <prefix>/lib.
    set_target_properties(neiro_core PROPERTIES
      BUILD_RPATH "${NEIRO_FFMPEG_ROOT}/lib")
  endif()
endif()

# Cross-platform checks. They link only the platform-neutral core; no backend,
# no device, no Windows header. On macOS/Linux the configure stops here until a
# backend exists, so a missing CoreAudio/AAudio implementation is explicit.
add_executable(policy_check policy_check.cpp)
target_link_libraries(policy_check PRIVATE neiro_core)
target_include_directories(policy_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hilbert_check hilbert_check.cpp)
target_link_libraries(hilbert_check PRIVATE neiro_core)
target_include_directories(hilbert_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
  target_link_options(policy_check PRIVATE "-static-libgcc" "-static-libstdc++")
  target_link_options(hilbert_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

# Backend selection. The core is platform-neutral; this picks the one OS
# boundary. WASAPI is the existing engine (nae.dll). CoreAudio/AAudio build a
# backend library plus the shared core checks; they do not contain a second DSP.
set(NEIRO_BACKEND "WASAPI" CACHE STRING "Audio backend: WASAPI, COREAUDIO, AAUDIO")
set_property(CACHE NEIRO_BACKEND PROPERTY STRINGS WASAPI COREAUDIO AAUDIO)

if(NEIRO_BACKEND STREQUAL "WASAPI" AND NOT WIN32)
  message(FATAL_ERROR "NEIRO_BACKEND=WASAPI requires Windows")
endif()
if(NEIRO_BACKEND STREQUAL "COREAUDIO" AND NOT APPLE)
  message(FATAL_ERROR "NEIRO_BACKEND=COREAUDIO requires macOS/iOS")
endif()
if(NEIRO_BACKEND STREQUAL "AAUDIO" AND NOT ANDROID)
  message(FATAL_ERROR "NEIRO_BACKEND=AAUDIO requires Android")
endif()

if(NEIRO_BACKEND STREQUAL "COREAUDIO")
  add_library(neiro_backend STATIC backend_coreaudio.cpp)
  target_link_libraries(neiro_backend PUBLIC neiro_core
                        "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
  target_include_directories(neiro_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  return()
endif()

if(NEIRO_BACKEND STREQUAL "AAUDIO")
  add_library(neiro_backend STATIC backend_aaudio.cpp)
  target_link_libraries(neiro_backend PUBLIC neiro_core aaudio)
  target_include_directories(neiro_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  return()
endif()

add_library(nae SHARED
  nae.cpp
  session.cpp
  backend_wasapi.cpp
)
target_link_libraries(nae PRIVATE neiro_core)
if(NEIRO_HAVE_FFMPEG)
  target_compile_definitions(nae PRIVATE NEIRO_HAVE_FFMPEG=1)
endif()

set_target_properties(nae PROPERTIES
  OUTPUT_NAME "nae"
  PREFIX ""            # nae.dll, not libnae.dll
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

target_compile_definitions(nae PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WIN32_LEAN_AND_MEAN)

# Debug info without changing optimisation. Needed because the engine is the
# only place a fault can land: it is loaded as a DLL into a Rust host, so the
# process dies without a message and a debugger is the only way to see where.
# -g only, deliberately — -O0 here would change the timing under the audio
# thread and hide whatever is being chased.
target_compile_options(nae PRIVATE -g)

# ---------------------------------------------------------------------------
# Export control.
#
# Only the nine NAE_* entry points may leave the DLL. Two toolchains, two
# mechanisms, one outcome:
#
#   GNU ld  -- a version script. `local: *;` hides everything not listed.
#   MSVC    -- a .def file. (A bare "/DEF:path" in LINK_FLAGS gets mangled into
#             a filesystem path by CMake on Windows and the link fails, so it
#             has to go through target_link_options.)
#
# Do not "simplify" this back to --output-def: that does not restrict exports at
# all, it left 8598 symbols visible and turned the entire C++ implementation
# into public ABI.
# ---------------------------------------------------------------------------
if(MSVC)
  target_link_options(nae PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/nae.def")
else()
  target_link_options(nae PRIVATE
    "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/nae.map"
  )
endif()

# **导出列表是链接的输入，所以必须是链接的依赖。**
#
# 不声明的话 ninja 认为 DLL 是最新的，改了列表直接 `no work to do`、不重链 ——
# 于是「加了一个导出，却到处都解析不到」看起来像代码问题，而 dll 里其实还是旧的
# 导出表。实测踩到：加了 `NAE_QueryPeaks` 的实现、编译过、链接过（`nae` 那个
# target 自己没动静），而 `peaks_check` 一直 `undefined reference`。
#
# `LINK_DEPENDS` 就是为这件事存在的：它不参与编译，只让链接器在这一项变化时重跑。
set_property(TARGET nae APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/nae.def"
  "${CMAKE_CURRENT_SOURCE_DIR}/nae.map")

target_include_directories(nae PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${NEIRO_SOXR_DIR}/src   # soxr.h, included by resampler.cpp
)
# The DSD decimation cascade the engine links in, enabled by NEIRO_DSD_DECODER=1.
  #
  # FFmpeg still opens the file and still supplies duration and tags -- those come from
  # its metadata dictionary, and re-implementing them would cost real work while losing
  # what the fact strip shows. Only the *samples* come from here.
  #
  # Behind an environment variable so one build can do A/B. A/B is the only way to say
  # whether the measured 66.02 dB actually reaches the real output path, rather than
  # existing only inside an instrument.
  target_sources(nae PRIVATE dsd_filter.cpp dsd_downrate.cpp)
target_link_libraries(nae PRIVATE
  mmdevapi      # WASAPI (IAudioClient etc. live in mmdevapi.dll)
  ole32
  uuid
  ksuser
  avrt
  propsys
  soxr          # sample rate conversion; see third_party/soxr
)

# Fold the MinGW runtime into the DLL. Otherwise nae.dll carries imports on
# libstdc++-6.dll and libgcc_s_seh-1.dll, and the host app has to ship and
# locate those two alongside it -- an easy way to ship a broken app, especially
# under a Tauri bundle where the DLL sits in a resource directory.
#
# Safe here because the ABI contract in nae.h forbids exceptions crossing the
# boundary, so there is no unwinder to share. If that ever changes, these flags
# have to go.
target_link_options(nae PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)

# ---------------------------------------------------------------------------
# Test harness. Links against the import library, not the DLL, and loads the
# DLL at runtime to exercise the real dynamic-loading path.
# ---------------------------------------------------------------------------
add_executable(nae_test test_main.cpp)
target_include_directories(nae_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nae_test PRIVATE nae)
add_dependencies(nae_test nae)

# The pure-tone path on its own. NAE_OpenExclusiveSine bypasses the decoder, the
# resampler and the dither, so a bubble here is in the write path and a clean one
# puts the fault in whatever feeds it. See sine_probe.cpp.
add_executable(sine_probe sine_probe.cpp)
target_include_directories(sine_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sine_probe PRIVATE nae)
add_dependencies(sine_probe nae)
add_custom_command(TARGET sine_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:sine_probe>
  COMMENT "Copying nae.dll next to the sine probe")

# Whether this device can carry DSD at all, via DoP.
  #
  # DoP packs the DSD bitstream into a 24-bit PCM carrier without decoding it, so
  # the ultrasonic noise that the decoder's resampler produces never exists. That
  # makes it the only approach that removes the noise floor at the source instead of
  # filtering it afterwards -- and DSD64's carrier is 352800 Hz, which is above what
  # this endpoint declares.
  #
  # So this measures the rate question *before* any packing code exists. Building a
  # DoP packer for a device that cannot open the carrier rate would be code that
  # cannot run, and the answer here decides whether it is worth writing.
  #
  # It has no result this machine can act on, so it returns 0 unconditionally and
  # prints its conclusion in prose.
  add_executable(dop_probe dop_probe.cpp)
  target_include_directories(dop_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(dop_probe PRIVATE nae)
  add_dependencies(dop_probe nae)
  add_custom_command(TARGET dop_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:dop_probe>
    COMMENT "Copying nae.dll next to the DoP probe")

# Reads a DSF container without FFmpeg, and decides bit order by measurement.
  #
  # The bit-order question comes first because a wrong bit order still plays: the
  # decoder emits plausible float audio, the DAC locks, the music comes out. The only
  # thing that changes is the shape of the noise-shaping slope, and that is directly
  # measurable on the 1-bit stream -- no device, no playback, no ears.
  #
  # Twilight Echo records the same class of fault from the other side: bit order and
  # byte order each reversed the wrong way, compounding into a full 16-bit time
  # reversal per frame. Local bit density survived, so nothing failed visibly.
  #
  # Deliberately links only nae.h for nothing -- this instrument does not use the
  # engine at all. FFmpeg's `dsd_lsbf_planar` is GPL and it also decides bit order for
  # us, which is exactly what a hand-written decoder has to be able to check itself
  # against.
  add_executable(dsd_probe dsd_probe.cpp)
  # FFmpeg 用来做交叉核对，不是用来解码——容器解析和比特展开都是本文件自己的代码。
  #
  # 这一步是关键：位序是「我的实现」与「实际在用的解码器」之间的一致性问题，而
  # 不是文档里的一句断言。只有两个独立实现逐比特相同才算锁死。
  target_include_directories(dsd_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} "${NEIRO_FFMPEG_ROOT}/include")
  target_link_directories(dsd_probe PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
  target_link_libraries(dsd_probe PRIVATE avformat avcodec avutil)

# The DSD decimation filter, prototyped and measured against the current path.
  #
  # The filter exists today -- inside FFmpeg's DSD decoder, which decimates 2822400 bps
  # to 352800 Hz and offers no way to change it. So the chain is two cascaded filters,
  # and a cascade's stopband is only as good as its weaker stage. What a hand-written
  # decoder changes is the shape of the chain: one decimation from 2822400 to 176400
  # (÷16) whose stopband can be designed, instead of ÷8 inside the codec followed by
  # ÷1.84 in soxr.
  #
  # Both paths terminate at the same intermediate rate on purpose, so the comparison
  # isolates the DSD stage. Comparing at 192 kHz would blend in soxr and leave it
  # undecided which stage owns the floor.
  add_executable(dsd_decim dsd_decim.cpp)
  target_include_directories(dsd_decim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} "${NEIRO_FFMPEG_ROOT}/include")
  target_link_directories(dsd_decim PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
  target_link_libraries(dsd_decim PRIVATE avformat avcodec avutil)

# The decimation filter as production code, plus the streaming check on it.
  #
  # `dsd_filter.cpp` is what the engine will actually use; `dsd_decim.cpp` keeps its own
  # copy of the design maths on purpose, because that is what makes it an instrument
  # rather than a test of itself. Sharing one implementation would mean a bug in the
  # filter design would be invisible -- both would agree.
  add_library(nae_dsd_filter STATIC dsd_filter.cpp)
  target_include_directories(nae_dsd_filter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} "${NEIRO_FFMPEG_ROOT}/include")

# Whether streaming the filter in arbitrary chunk sizes gives the same samples as
  # running it over one contiguous buffer.
  #
  # This is the only check that can catch a dropped FIR history, and a dropped history
  # is invisible to every other check available: the output is the right length, the
  # right level, and has a step discontinuity at every DSF block boundary -- 14.6 ms
  # apart. Peak checks pass. Duration checks pass. Only a sample-for-sample comparison
  # against the whole-buffer result, or listening, finds it.
  add_executable(dsd_filter_check dsd_filter_check.cpp)

# Re-derives the cascade's band levels on a real DSD file, through the code the engine
  # uses, with sentinel detection.
  #
  # Two numbers disagreed by ~170 dB: `dsd_decim` said the cascade reaches -220.39 dBFS at
  # 40-80 kHz, and the engine's A/B said the same path sits 2.25 dB below the music at
  # 24-48 kHz. And `dsd_decim`'s figure may not be a measurement at all -- its `bandRms`
  # returns -200.0 for a band with no bins, and clamps the floor at log10(1e-12) = -240.00,
  # and the printed numbers land on those magnitudes.
  #
  # So the question is which of the two is right, and the answer decides whether the work
  # is a new instrument or a new investigation. **An empty band is a FAIL here, never a
  # number**: a -200 reads as "extremely quiet", which is the one conclusion it cannot
  # support.
  add_executable(dsd_band_check dsd_band_check.cpp)

# Plays a file for N seconds, for listening.
  #
  # Because the last step on the DSD plateau question is ears: the component measures a
  # -156 dB stopband, the engine path shows an unexplained plateau from 8 to 44 kHz, and the
  # frequency-domain evidence has run out.
  add_executable(decode_path_check decode_path_check.cpp)
target_include_directories(decode_path_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(decode_path_check PRIVATE nae)
add_dependencies(decode_path_check nae)
add_custom_command(TARGET decode_path_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:decode_path_check>
  COMMENT "Copying nae.dll next to the decode path check")

add_executable(make_dsd128 make_dsd128.cpp)

add_executable(open_check open_check.cpp)
target_include_directories(open_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(open_check PRIVATE nae)
add_dependencies(open_check nae)
add_custom_command(TARGET open_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:open_check>
  COMMENT "Copying nae.dll next to the Unicode path check")

# 能力声明 vs 真实的导出表。
#
# 存在的理由：`NAE_GetEngineCapabilities` 曾经声称 `shareModes` 里有 `shared`，
# 而引擎没有打开 shared 流的代码路径。字符串改成表驱动之后，「只加表项不改
# ABI」这种漂移仍然可能发生，所以这个仪器拿声明去问 DLL 真实的导出表。
# 断言的期望值来自导出表，不是来自生成那段 JSON 的代码。
add_executable(capability_check capability_check.cpp)
  target_include_directories(capability_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(capability_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_custom_command(TARGET capability_check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:capability_check>
    COMMENT "Copying nae.dll next to the capability check")

# shared 模式能不能真的开出声。
#
# 加了 `NAE_OpenFileShared` 之后，「接上了导出符号」和「能用」是两件事：
# 导出表只说明符号在，而一个从来没人调用过的入口可能根本打不开设备。这个仪器
# 真的开一次、播一段、读引擎自报的信号路径。
#
# 断言的期望值来自 shared 模式的定义（混音器在通路上 -> bitPerfect 必须是 0），
# 不是来自引擎的输出。开不开得了**不断言**——失败本身就是关于这台机器此刻的答案。
add_executable(shared_check shared_check.cpp)
  target_include_directories(shared_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(shared_check PRIVATE nae)
  add_dependencies(shared_check nae)
  add_custom_command(TARGET shared_check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:shared_check>
    COMMENT "Copying nae.dll next to the shared-mode check")

add_executable(play play.cpp)
  target_include_directories(play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(play PRIVATE nae)
  add_dependencies(play nae)
  add_custom_command(TARGET play POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:play>
    COMMENT "Copying nae.dll next to the player")
  target_include_directories(dsd_band_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(dsd_band_check PRIVATE nae_dsd_filter)

# Whether the own decoder's 66 dB actually reaches the output, measured in the engine.
  #
  # The instrument measured -220.39 vs -154.37 dBFS at 40-80 kHz, but that was inside
  # the instrument: no engine, no soxr, no WASAPI. Between the two there are three
  # things that can each eat the difference, and only the engine has them --
  # the engine's soxr folding, whether the engine was told the rate actually changed,
  # and whether the frame accounting still balances after a decimation stage.
  add_executable(dsd_path_ab dsd_path_ab.cpp)
  target_include_directories(dsd_path_ab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(dsd_path_ab PRIVATE nae)
  add_dependencies(dsd_path_ab nae)
  add_custom_command(TARGET dsd_path_ab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:dsd_path_ab>
    COMMENT "Copying nae.dll next to the DSD A/B instrument")
  target_include_directories(dsd_filter_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(dsd_filter_check PRIVATE nae_dsd_filter)

# The output spectrum tap, and the instrument that checks it.
#
# The probe exists because a spectrum display fed the wrong window, channel,
# scaling or rate looks entirely plausible and measures something else. It asserts
# against NAE_OpenExclusiveSine's own 1 kHz / -20 dBFS tone and prints the ratio,
# so a 3 dB scaling error or a wrong rate is a number rather than a curve that
# looked right. Its file mode reads the same tap on a real file, which is the only
# way to see what the engine produced *after* resampling.
add_executable(spectrum_probe spectrum_probe.cpp)
target_include_directories(spectrum_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spectrum_probe PRIVATE nae)
add_dependencies(spectrum_probe nae)
add_custom_command(TARGET spectrum_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:spectrum_probe>
  COMMENT "Copying nae.dll next to the spectrum probe")

# The harness resolves the DLL relative to its own working directory.
add_custom_command(TARGET nae_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:nae_test>
  COMMENT "Copying nae.dll next to the test harness"
)

# ---------------------------------------------------------------------------
# Format matrix probe.
#
# Separate from the DLL on purpose: it links the WASAPI headers directly so it
# can inspect the raw PKEY_AudioEngine_DeviceFormat blob, which the engine's C
# ABI deliberately hides. It exists to tell two failures apart that look
# identical from outside — "another app holds the endpoint" and "we asked for a
# format the driver rejects" both surface as AUDCLNT_E_ENDPOINT_CREATE_FAILED.
# Trying a matrix of formats settles it: if none work, it is contention; if some
# do, it is our format negotiation.
# ---------------------------------------------------------------------------
add_executable(format_matrix format_matrix.cpp)
target_include_directories(format_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(format_matrix PRIVATE
  mmdevapi ole32 uuid ksuser avrt propsys
)
target_compile_definitions(format_matrix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# Resampler check.
#
# Separate from the DLL so it can call the C++ wrapper directly and inspect the
# samples. A resampler is the easiest component in the project to get wrong in a
# way that still looks fine: right number of frames, plausible noise. So this
# measures the tone with a Goertzel filter and checks the frequency and level
# survived, rather than checking only the length.
# ---------------------------------------------------------------------------
add_executable(resample_check resample_check.cpp resampler.cpp)
target_include_directories(resample_check PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${NEIRO_SOXR_DIR}/src   # soxr.h, which the wrapper includes
)
target_link_libraries(resample_check PRIVATE soxr)
set_target_properties(resample_check PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Decode check.
#
# Only built when FFmpeg is present. Measures the decoded samples — rate,
# duration, level, and that a known tone is still that tone — because a decoder
# returning the right number of frames of plausible noise is the failure mode
# that matters.
# ---------------------------------------------------------------------------
if(NEIRO_HAVE_FFMPEG)
  # `dsd_filter.cpp` 必须一起链上：`decoder.cpp` 在加了 DSD 支持之后引用了
  # `neiro::dsd::{Cascade,Stage,designDecimationFilter}`，而那些符号只在这里。
  # 少了它的后果不是「DSD 不工作」而是**链接直接失败** —— 所以这个 target 以及
  # 另外三个同样单独链 `decoder.cpp` 的（tail_check / splice_check / dsd_spectrum）
  # 在 `build/` 里一个 exe 都没有。实测：修之前 `cmake --build build --target
  # decode_check` 报 undefined reference 到 `neiro::dsd::Cascade::configure` 等 6 个符号。
  #
  # 只有这一个被修并**跑过**（`scripts/probe-timeline-decode-cost.mjs` 用它量整首解码
  # 的耗时）。另外三个是同一种断法，但我没有构建或运行它们，所以不声称修好了。
  add_executable(decode_check decode_check.cpp decoder.cpp dsd_filter.cpp)
  target_include_directories(decode_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${NEIRO_FFMPEG_ROOT}/include)
  target_link_directories(decode_check PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
  target_link_libraries(decode_check PRIVATE avformat avcodec swresample avutil)

# Nothing lost at the end of a file through `readExact`.
  #
  # Separate from decode_check because that one drains with `read()`, which cannot
  # lose samples by construction. `readExact` is the path the engine uses and the
  # one that can truncate a tail, double-hand an excess, or make `framesRead()`
  # disagree with what was actually delivered -- which is a seek bug wearing a
  # correct-audio costume.
  add_executable(tail_check tail_check.cpp decoder.cpp dsd_filter.cpp)
  target_include_directories(tail_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${NEIRO_FFMPEG_ROOT}/include)
  target_link_directories(tail_check PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
  target_link_libraries(tail_check PRIVATE avformat avcodec swresample avutil)

  # The two edges PAUSE-RESUME-SPEC.md lists as untested: seek while paused, and a
# track change while paused.
  #
  # Separate from the existing stress loops because those test the pause/play
  # boundary itself, which is measured and clean. These test what happens to a fifo
  # that was filled *before* a pause and then has to be replaced -- a stale-buffer
  # leak is inaudible in a level meter and obvious in the ear, so it needs a check
  # that compares position against content rather than either alone.
  add_executable(pause_edge_check pause_edge_check.cpp)
  target_include_directories(pause_edge_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(pause_edge_check PRIVATE nae)
  add_dependencies(pause_edge_check nae)
  add_custom_command(TARGET pause_edge_check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:pause_edge_check>
    COMMENT "Copying nae.dll next to the pause edge check")

  # 实测 `NAE_QueryPeaks`：形状、值域、**缓存命中**、参数校验。
  #
  # 链 `nae` 而不是 `decoder.cpp` —— 这一点是刻意的：peaks 那一遍走的就是引擎自己
  # 那条路（`handle->filePath` + 一个独立的 `neiro::Decoder`），把它拆出来单独链
  # 就等于量了另一个东西。顺带也避开了 `decoder.cpp` 那个「直链就得同时带
  # `dsd_filter.cpp`」的坑（见上面 decode_check 那段）。
  #
  # 它**不核对 peaks 的数值**：那需要一份独立算出来的参照。数值正确性没有验，
  # 已在 `peaks_check.cpp` 顶部写明。
  add_executable(peaks_check peaks_check.cpp)
  target_include_directories(peaks_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(peaks_check PRIVATE nae)
  add_dependencies(peaks_check nae)
  add_custom_command(TARGET peaks_check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nae> $<TARGET_FILE_DIR:peaks_check>
    COMMENT "Copying nae.dll next to peaks_check")

# Seams between decoded blocks. Separate from decode_check because that one asks
# "is the decode correct in aggregate" -- right peak, right duration, right sample
# count -- and a stream can satisfy every one of those while stepping at every block
# boundary. A step is a click, and several clicks a second is heard as bubbling.
# Needs no device, so it runs anywhere.
add_executable(splice_check splice_check.cpp decoder.cpp dsd_filter.cpp)
target_include_directories(splice_check PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${NEIRO_FFMPEG_ROOT}/include)
target_link_directories(splice_check PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
target_link_libraries(splice_check PRIVATE avformat avcodec swresample avutil)
set_target_properties(splice_check PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  # Plays a real file through the exclusive device in host-sized slices. Links
  # the whole nae DLL rather than just the decoder, because what it checks is the
  # render loop's state across calls -- a property of nae, not of the decoder.
  add_executable(file_check file_check.cpp)
  target_link_libraries(file_check PRIVATE nae)

  # `bit_perfect` against the source, not only the device. Generates its own
  # 16/24/32-bit fixtures so it runs without the caller supplying anything.
  add_executable(source_width_check source_width_check.cpp)
  target_link_libraries(source_width_check PRIVATE nae)

  # The struct-size contract: a caller's declared size is honoured, not
  # overwritten. Needs no device and no file.
  add_executable(struct_size_check struct_size_check.cpp)
  target_link_libraries(struct_size_check PRIVATE nae)

  # Steady-state pipeline latency, and the CPU it costs.
  #
  # Reports the FIFO depth the pipeline holds, because that is the latency the
  # engine adds on top of the device's own buffer -- and because it is a number
  # that responds to policy (refill threshold, resampler block size) rather than
  # to a constant, so it is the thing worth tuning.
  add_executable(latency_bench latency_bench.cpp)
  target_link_libraries(latency_bench PRIVATE nae)

  # Sustained playback: a time series rather than a summary.
  #
  # latency_bench reports a steady state over a few seconds, which answers the
  # latency question and hides the accumulation one -- a FIFO creeping up and a
  # FIFO that jumped once at start-up give nearly the same median, and a drift
  # that ramps linearly looks like a small constant bias until the two are
  # separated. This samples every interval and fits the growth slopes.
  add_executable(sustain_check sustain_check.cpp)
  target_link_libraries(sustain_check PRIVATE nae)

  # Frequency-domain characterisation of the decode path.
  #
  # Separate from decode_check because it answers a different question. The
  # time-domain checks cannot see decimation aliasing: folded ultrasonic noise
  # still gives the right peak, the right duration and the right sample count,
  # and is inaudible to every assertion they make.
  add_executable(dsd_spectrum dsd_spectrum.cpp decoder.cpp dsd_filter.cpp)
  target_include_directories(dsd_spectrum PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} "${NEIRO_FFMPEG_ROOT}/include")
  target_link_directories(dsd_spectrum PRIVATE "${NEIRO_FFMPEG_ROOT}/lib")
  target_link_libraries(dsd_spectrum PRIVATE avformat avcodec swresample avutil)
endif()

# ---------------------------------------------------------------------------
# Rapid reopen check.
#
# The app dies when a play is dispatched while a render is in flight: the audio
# thread closes the exclusive stream and reopens the same endpoint within
# microseconds. No harness had exercised that transition, so it could be a
# driver that cannot release and re-take an endpoint promptly — which would
# present as a silent process death rather than an error code.
# ---------------------------------------------------------------------------
add_executable(reopen_check reopen_check.cpp)
target_include_directories(reopen_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(reopen_check PRIVATE
  mmdevapi ole32 uuid ksuser avrt propsys
)
target_compile_definitions(reopen_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET reopen_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:reopen_check>
)

# ---------------------------------------------------------------------------
# Output mode check.
#
# Resolves NAE_OpenExclusive with GetProcAddress and drives the real WASAPI
# path, so it verifies the symbol is genuinely exported as well as correct.
# The assertions that matter are about honesty: `bitPerfect` must be false
# whenever the rates differ, because a player that resamples and still claims
# bit-perfect output is worse than one that never resamples.
# ---------------------------------------------------------------------------
add_executable(mode_check mode_check.cpp)
target_include_directories(mode_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mode_check PRIVATE
  mmdevapi ole32 uuid ksuser avrt propsys
)
# Format negotiation, driven through a fake endpoint.
#
# Links the header directly and needs none of the engine: `negotiateExclusive` is
# a template over an injected predicate, so this binary is the specification of
# the candidate order rather than a test of the DLL. Deliberately no
# `add_dependencies(... nae)` and no copy of nae.dll -- if it ever grows a link
# against the engine, it has stopped testing the ordering and started testing the
# plumbing.
add_executable(negotiation_check negotiation_check.cpp)
target_include_directories(negotiation_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(negotiation_check PRIVATE
  mmdevapi ole32 uuid ksuser avrt propsys
)
target_compile_definitions(negotiation_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(mode_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET mode_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:mode_check>
  COMMENT "Copying nae.dll next to the mode check"
)

# ---------------------------------------------------------------------------
# Dither check.
#
# Dither cannot be verified by listening to it — a subtly wrong implementation
# still sounds like music. So this measures the property that distinguishes it
# from plain rounding: a constant sub-LSB signal must produce a spread of output
# values, with no bias, at an amplitude near one LSB. Rectangular dither and
# correct TPDF dither both pass a naive "is there noise" test, so the error
# amplitude is checked against the theoretical TPDF value too.
# ---------------------------------------------------------------------------
add_executable(dither_check dither_check.cpp dither.cpp)
target_include_directories(dither_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dither_check PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# The same question asked of a real endpoint, printing raw HRESULTs.
#
# A diagnostic and not a check: it opens the device, so it cannot run while anything
# else holds it, and it is deliberately not in run-checks.mjs. It exists because
# `negotiateExclusive` collapses sixty refusals into one boolean, and when that
# boolean is wrong on real hardware the only question worth asking is "what did the
# device actually say", which the engine's own error path summarises away.
add_executable(negotiation_live negotiation_live.cpp)
target_include_directories(negotiation_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(negotiation_live PRIVATE mmdevapi ole32 uuid ksuser avrt propsys)
target_compile_definitions(negotiation_live PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# Session check. The session thread owns the handle; this drives it through
# play/pause/seek/stop and measures the state the listener gets, not whether a
# function returned zero.
# ---------------------------------------------------------------------------
add_executable(session_check session_check.cpp)
target_include_directories(session_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_check PRIVATE nae shell32)
add_dependencies(session_check nae)
target_link_options(session_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET session_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:session_check>
  COMMENT "Copying nae.dll next to the session check")

# ---------------------------------------------------------------------------
# The CLI host. Same DLL and same commands as the Rust binding; no second
# engine path. It is a dev/test host, not a UI.
# ---------------------------------------------------------------------------
add_executable(neiro_cli cli.cpp)
target_include_directories(neiro_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(neiro_cli PRIVATE nae shell32)
add_dependencies(neiro_cli nae)
target_link_options(neiro_cli PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET neiro_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:neiro_cli>
  COMMENT "Copying nae.dll next to the CLI")

# Digital volume: two fresh handles, same first 500 ms, unity vs 0.5. The
# spectrum tap measures the samples after the 20 ms ramp; the signal path must
# also report the volume reason.
add_executable(volume_check volume_check.cpp)
target_include_directories(volume_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(volume_check PRIVATE nae shell32)
add_dependencies(volume_check nae)
target_link_options(volume_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET volume_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:volume_check>
  COMMENT "Copying nae.dll next to the volume check")

# Equaliser: a -12 dB peaking band at the tone's own bin. Measured on the
# spectrum tap, disabled again, and measured again; a filter that is stuck on
# would pass the first half and fail the second.
add_executable(eq_check eq_check.cpp)
target_include_directories(eq_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eq_check PRIVATE nae shell32)
add_dependencies(eq_check nae)
target_link_options(eq_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET eq_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:eq_check>
  COMMENT "Copying nae.dll next to the EQ check")

# Crossfeed: a 300 Hz tone in the right channel only, measured in the left. Off
# must be silence; on must put the delayed, low-passed copy there; off again must
# return to silence.
add_executable(crossfeed_check crossfeed_check.cpp)
target_include_directories(crossfeed_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crossfeed_check PRIVATE nae shell32)
add_dependencies(crossfeed_check nae)
target_link_options(crossfeed_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET crossfeed_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:crossfeed_check>
  COMMENT "Copying nae.dll next to the crossfeed check")

# ReplayGain: the fixture carries a -6 dB track tag and a -3 dB album tag. The
# tone's bin is measured at each policy, then the policy is turned off.
add_executable(replaygain_check replaygain_check.cpp)
target_include_directories(replaygain_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(replaygain_check PRIVATE nae shell32)
add_dependencies(replaygain_check nae)
target_link_options(replaygain_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET replaygain_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:replaygain_check>
  COMMENT "Copying nae.dll next to the ReplayGain check")

# Gapless: two same-format files, the engine preloads the second and the session
# queue advances the serial. The engine's underrun/pad counters must not move at
# the seam; the session must stay PLAYING through it.
add_executable(skip_check skip_check.cpp)
target_include_directories(skip_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(skip_check PRIVATE nae)
add_dependencies(skip_check nae)

add_executable(gapless_check gapless_check.cpp)
target_include_directories(gapless_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gapless_check PRIVATE nae shell32)
add_dependencies(gapless_check nae)
target_link_options(gapless_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET gapless_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:gapless_check>
  COMMENT "Copying nae.dll next to the gapless check")

# Convolver: a generated 0.5 delta IR. The measured tone must halve while it is
# enabled and return when it is disabled.
add_executable(convolver_check convolver_check.cpp)
target_include_directories(convolver_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(convolver_check PRIVATE nae shell32)
add_dependencies(convolver_check nae)
target_link_options(convolver_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET convolver_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:convolver_check>
  COMMENT "Copying nae.dll next to the convolver check")

# Crossfade: two tones, 0.5 s fade. Both bins must be present at the midpoint;
# after the swap the next track starts about 0.5 s in, with no gap.
add_executable(crossfade_check crossfade_check.cpp)
target_include_directories(crossfade_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crossfade_check PRIVATE nae shell32)
add_dependencies(crossfade_check nae)
target_link_options(crossfade_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET crossfade_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:crossfade_check>
  COMMENT "Copying nae.dll next to the crossfade check")

# The registry must agree with the typed APIs: a per-band write has to be visible
# through NAE_SessionGetEq, and an open-time value must say reopen rather than
# pretending to apply.
add_executable(param_check param_check.cpp)
target_include_directories(param_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(param_check PRIVATE nae shell32)
add_dependencies(param_check nae)
target_link_options(param_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET param_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:param_check>
  COMMENT "Copying nae.dll next to the parameter check")

# DoP packing and container parsing, without a device. A DFF/DSF file is read
# and the 24-bit words are compared to the bytes it actually stores.
add_executable(dop_check dop_check.cpp dsd_downrate.cpp dsd_filter.cpp)
target_include_directories(dop_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dop_check PRIVATE nae shell32)
add_dependencies(dop_check nae)
target_link_options(dop_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET dop_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:dop_check>
  COMMENT "Copying nae.dll next to the DoP check")

# The decode-ahead producer must be active, not the inline fallback.
add_executable(decode_thread_check decode_thread_check.cpp)
target_include_directories(decode_thread_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(decode_thread_check PRIVATE nae shell32)
add_dependencies(decode_thread_check nae)
target_link_options(decode_thread_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET decode_thread_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:decode_thread_check>
  COMMENT "Copying nae.dll next to the decode-thread check")

# Analysis plane: one decode -> format, full tags, loudness/DR, peaks, BPM.
add_executable(analysis_check analysis_check.cpp)
target_include_directories(analysis_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(analysis_check PRIVATE nae shell32)
add_dependencies(analysis_check nae)
target_link_options(analysis_check PRIVATE
  "-static-libgcc"
  "-static-libstdc++"
  "-Wl,-Bstatic,--whole-archive,-lstdc++,-lwinpthread,--no-whole-archive,-Bdynamic"
)
add_custom_command(TARGET analysis_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nae> $<TARGET_FILE_DIR:analysis_check>
  COMMENT "Copying nae.dll next to the analysis check")

add_executable(caps_check caps_check.cpp)
target_include_directories(caps_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(caps_check PRIVATE nae)
add_dependencies(caps_check nae)

add_executable(session_policy_check session_policy_check.cpp)
target_include_directories(session_policy_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_policy_check PRIVATE nae)
add_dependencies(session_policy_check nae)

add_executable(routing_check routing_check.cpp)
target_link_libraries(routing_check PRIVATE neiro_core)
target_include_directories(routing_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(routing_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(routing_play_check routing_play_check.cpp)
target_include_directories(routing_play_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(routing_play_check PRIVATE nae)
add_dependencies(routing_play_check nae)

add_executable(session_routing_check session_routing_check.cpp)
target_include_directories(session_routing_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_routing_check PRIVATE nae)
add_dependencies(session_routing_check nae)

add_executable(session_dsd_check session_dsd_check.cpp)
target_include_directories(session_dsd_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_dsd_check PRIVATE nae)
add_dependencies(session_dsd_check nae)

add_executable(channel_check channel_check.cpp)
target_include_directories(channel_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(channel_check PRIVATE nae)
add_dependencies(channel_check nae)

add_executable(clip_guard_check clip_guard_check.cpp)
target_link_libraries(clip_guard_check PRIVATE neiro_core)
target_include_directories(clip_guard_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(clip_guard_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(native_dsd_sink_check native_dsd_sink_check.cpp dsd_downrate.cpp dsd_filter.cpp)
target_include_directories(native_dsd_sink_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(native_dsd_sink_check PRIVATE nae)
add_dependencies(native_dsd_sink_check nae)
if(WIN32)
  target_link_options(native_dsd_sink_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(dsd_downrate_check dsd_downrate_check.cpp dsd_downrate.cpp dsd_filter.cpp)
target_include_directories(dsd_downrate_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(dsd_downrate_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(linear_phase_eq_check linear_phase_eq_check.cpp linear_phase_eq.cpp partitioned_convolver.cpp)
target_include_directories(linear_phase_eq_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(linear_phase_eq_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(loudness_check loudness_check.cpp loudness.cpp)
target_include_directories(loudness_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(loudness_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(partitioned_convolver_check partitioned_convolver_check.cpp partitioned_convolver.cpp)
target_include_directories(partitioned_convolver_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(partitioned_convolver_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

add_executable(backend_check backend_check.cpp backend_wasapi.cpp)
target_include_directories(backend_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(backend_check PRIVATE nae)
add_dependencies(backend_check nae)

add_executable(buffer_policy_check buffer_policy_check.cpp)
target_include_directories(buffer_policy_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(buffer_policy_check PRIVATE nae)
add_dependencies(buffer_policy_check nae)

add_executable(bass_check bass_check.cpp)
target_link_libraries(bass_check PRIVATE neiro_core)
target_include_directories(bass_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_options(bass_check PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
