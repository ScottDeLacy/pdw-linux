cmake_minimum_required(VERSION 3.16)
project(pdw-linux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Upstream assumes signed `char` (x86 default; arm64 Linux/macOS differ), so it is
# pinned explicitly.
set(PDW_CORE_FLAGS -fsigned-char -fpermissive -Wno-write-strings
                   -Wno-narrowing -Wno-format-security -Wno-deprecated-declarations)

add_library(pdw STATIC
  libpdw/src/pocsag.cpp
  libpdw/src/flex.cpp
  libpdw/src/upstream_misc.cpp
  libpdw/src/upstream_audio.cpp
  libpdw/src/upstream_decode_globals.cpp
  libpdw/src/upstream_settings.cpp
  libpdw/src/upstream_language.cpp
  libpdw/src/upstream_gfx.cpp
  libpdw/src/win32_shim.cpp
  libpdw/src/mail.cpp
  libpdw/src/support.cpp)
target_include_directories(pdw PUBLIC libpdw/include PRIVATE libpdw)

# TLS for SMTP (pdw.ini [SMTP] SSL=1) when OpenSSL is available.
find_package(OpenSSL QUIET)
find_package(Threads REQUIRED)
target_link_libraries(pdw PUBLIC Threads::Threads)
if(OpenSSL_FOUND)
  target_compile_definitions(pdw PRIVATE PDW_HAVE_OPENSSL)
  target_link_libraries(pdw PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(pdw PRIVATE ${PDW_CORE_FLAGS})

add_executable(pdw-cli cli/main.cpp)
target_link_libraries(pdw-cli PRIVATE pdw)
target_compile_options(pdw-cli PRIVATE -fsigned-char -Wall -Wextra)

enable_testing()
add_subdirectory(tests)
