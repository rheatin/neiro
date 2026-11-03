cmake_minimum_required(VERSION 3.20)
project(wasapi_probe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wasapi_probe wasapi_probe.cpp)

target_link_libraries(wasapi_probe PRIVATE
  # WASAPI's IAudioClient / IAudioRenderClient live in mmdevapi.dll, which
  # MinGW does ship an import library for. There is no libaudio.a.
  mmdevapi
  ole32
  uuid
  ksuser
  avrt
  propsys
)
