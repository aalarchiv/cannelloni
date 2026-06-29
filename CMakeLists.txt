cmake_minimum_required(VERSION 3.15)
project(cannellonis)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

if(NOT PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  # Git auto-ignore out-of-source build directory
  file(GENERATE OUTPUT .gitignore CONTENT "*")
endif()

# Create compile_commands.json file
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(SCTP_SUPPORT "SCTP_SUPPORT" ON)
option(AVAHI_SUPPORT "AVAHI_SUPPORT" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")

if(SCTP_SUPPORT)
  include(FindSCTP)
else(SCTP_SUPPORT)
  message(STATUS "Building cannellonis without SCTP support (SCTP_SUPPORT=OFF)")
endif(SCTP_SUPPORT)

if(NOT SCTP_FOUND AND SCTP_SUPPORT)
  set(SCTP_SUPPORT OFF)
  message(STATUS "SCTP not found. cannellonis will be build without SCTP support.")
  message(STATUS "Install lksctp-tools for SCTP.")
endif(NOT SCTP_FOUND AND SCTP_SUPPORT)

# Optional mDNS/Avahi zeroconf peer discovery (cannelloni-84a.7). Mirrors the
# SCTP_SUPPORT pattern: on by default, but silently disabled (the feature is
# fully optional) if the Avahi client library is not present.
if(AVAHI_SUPPORT)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(AVAHI IMPORTED_TARGET avahi-client)
  endif()
  if(NOT AVAHI_FOUND)
    set(AVAHI_SUPPORT OFF)
    message(STATUS "Avahi (avahi-client) not found. Building without mDNS discovery.")
    message(STATUS "Install libavahi-client for mDNS peer discovery.")
  endif()
else(AVAHI_SUPPORT)
  message(STATUS "Building cannellonis without mDNS discovery (AVAHI_SUPPORT=OFF)")
endif(AVAHI_SUPPORT)

CONFIGURE_FILE(
  ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/config.h
)

include_directories(BEFORE ${CMAKE_CURRENT_BINARY_DIR})

add_executable(cannellonis cannelloni.cpp)
add_library(addsources STATIC
            connection.cpp
            framebuffer.cpp
            inet_address.cpp
            thread.cpp
            timer.cpp
            udpthread.cpp
            tcpthread.cpp
            tcp_client_thread.cpp
            tcp_server_thread.cpp
            canthread.cpp
            router.cpp
            peerregistry.cpp
            mdns.cpp)

add_library(cannelloni-common SHARED
            parser.cpp
            decoder.cpp)

add_library(cannelloni-common-static STATIC
            parser.cpp
            decoder.cpp)

set_target_properties ( cannelloni-common
  PROPERTIES
  VERSION 0.0.1
  SOVERSION 0
)

include(GNUInstallDirs)

if(SCTP_SUPPORT)
    add_library(sctpthread STATIC sctpthread.cpp)
    target_link_libraries(sctpthread addsources sctp)
    target_link_libraries(addsources sctpthread)
endif(SCTP_SUPPORT)
# mdns.cpp is compiled unconditionally but is an empty translation unit unless
# AVAHI_SUPPORT is defined (config.h), so only link the Avahi client library when
# the feature is enabled. The dependency propagates to the cannellonis executable.
if(AVAHI_SUPPORT)
    target_link_libraries(addsources PkgConfig::AVAHI)
endif(AVAHI_SUPPORT)
set_target_properties(addsources PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cannellonis addsources cannelloni-common-static pthread)
target_compile_features(cannellonis PRIVATE cxx_auto_type)
target_compile_features(addsources PRIVATE cxx_auto_type)

install(TARGETS cannellonis DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS cannelloni-common DESTINATION ${CMAKE_INSTALL_LIBDIR})
