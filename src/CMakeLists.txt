cmake_minimum_required(VERSION 3.22)
project(ech_http_rn LANGUAGES C CXX)

# 去 Dart 化后的 ech_http 原生引擎，脱离 Dart build hook 独立编译。
# 用法: cmake -S src -B build -G Ninja -DEH_DEPS_ROOT=<预编译 SDK 目录>
# 依赖 SDK 需提供 include/{curl,openssl} + lib/{libcurl,libssl,libcrypto,libzlib}.a

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT DEFINED EH_DEPS_ROOT)
  message(FATAL_ERROR "必须提供 -DEH_DEPS_ROOT=<含 cmake/EchHttpDeps.cmake 的预编译 SDK 目录>")
endif()
include("${EH_DEPS_ROOT}/cmake/EchHttpDeps.cmake")

# CA 根证书编译进二进制，运行时不需要外部证书文件
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cacert.pem" EH_CA_PEM)
configure_file(ca_bundle.h.in "${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.h" @ONLY)

add_library(ech_http SHARED ech_http.cpp)
# imported 目标默认走 -isystem，会被宿主机的 OpenSSL 头抢占，强制用本 SDK 的头
set_target_properties(ech_http PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
target_include_directories(ech_http PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ech_http PRIVATE EchHttp::curl)
find_package(Threads REQUIRED)
target_link_libraries(ech_http PRIVATE Threads::Threads)

if(ANDROID)
  target_link_options(ech_http PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,--exclude-libs,ALL"
    "-Wl,--strip-debug")
elseif(UNIX AND NOT APPLE)
  target_link_options(ech_http PRIVATE "-Wl,--exclude-libs,ALL")
endif()
if(APPLE)
  set_target_properties(ech_http PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON)
endif()

set_target_properties(ech_http PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

# 主机侧冒烟程序：用最朴素的 C 宿主驱动引擎，证明运行时不依赖 Dart
if(NOT ANDROID)
  add_executable(eh_smoke eh_smoke.c)
  target_include_directories(eh_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(eh_smoke PRIVATE ech_http)
endif()
