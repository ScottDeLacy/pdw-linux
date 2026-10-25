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

add_library(pdw_core STATIC
  libpdw/src/pocsag.cpp
  libpdw/src/flex.cpp
  libpdw/src/acars.cpp
  libpdw/src/mobitex.cpp
  libpdw/src/ermes.cpp
  libpdw/src/settings_table.cpp
  libpdw/src/upstream_misc.cpp
  libpdw/src/upstream_audio.cpp
  libpdw/src/upstream_decode_globals.cpp
  libpdw/src/upstream_settings.cpp
  libpdw/src/upstream_language.cpp
  libpdw/src/upstream_gfx.cpp
  libpdw/src/win32_shim.cpp
  libpdw/src/mail.cpp
  libpdw/src/support.cpp)
target_include_directories(pdw_core PUBLIC libpdw/include PRIVATE libpdw)

# TLS for SMTP (pdw.ini [SMTP] SSL=1) when OpenSSL is available.
option(PDW_USE_OPENSSL "Use OpenSSL for TLS in the e-mail forwarder when found" ON)
if(PDW_USE_OPENSSL)
  find_package(OpenSSL QUIET)
endif()
find_package(Threads REQUIRED)
target_link_libraries(pdw_core PUBLIC Threads::Threads)
if(OpenSSL_FOUND)
  target_compile_definitions(pdw_core PRIVATE PDW_HAVE_OPENSSL)
  target_link_libraries(pdw_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(pdw_core PRIVATE ${PDW_CORE_FLAGS})

add_executable(pdw-cli cli/main.cpp)
target_link_libraries(pdw-cli PRIVATE pdw_core)
target_compile_options(pdw-cli PRIVATE -fsigned-char -Wall -Wextra)


# ---- Qt front end ---------------------------------------------------------------------------
option(PDW_BUILD_GUI "Build the Qt desktop front end" ON)
if(PDW_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets Multimedia QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    qt_add_resources(PDW_QRC gui/res/resources.qrc)
    add_executable(pdw
      gui/main.cpp
      gui/mainwindow.cpp
      gui/panewidget.cpp
      gui/inputsource.cpp
      gui/rcdialog.cpp
      gui/dialogs.cpp
      gui/dialogs2.cpp
      gui/dialogs_filters.cpp
      gui/dialogshot.cpp
      gui/selftest.cpp
      gui/generated/rc_data.cpp
      ${PDW_QRC})
    target_include_directories(pdw PRIVATE gui)
    target_link_libraries(pdw PRIVATE pdw_core Qt6::Widgets Qt6::Multimedia)
  else()
    message(STATUS "Qt6 not found: building the command-line decoder only")
  endif()
endif()

enable_testing()
add_subdirectory(tests)

# ---- install ----------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS pdw-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET pdw)
  install(TARGETS pdw RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(UNIX AND NOT APPLE)
    install(FILES packaging/pdw.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  endif()
endif()
install(FILES data/language.df data/base-ids.txt data/PDW.pdf DESTINATION ${CMAKE_INSTALL_DATADIR}/pdw)
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
