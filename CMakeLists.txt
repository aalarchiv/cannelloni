cmake_minimum_required(VERSION 3.15)
project(cannelloni)

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
  message(STATUS "Building cannelloni without SCTP support (SCTP_SUPPORT=OFF)")
endif(SCTP_SUPPORT)

if(NOT SCTP_FOUND AND SCTP_SUPPORT)
  set(SCTP_SUPPORT OFF)
  message(STATUS "SCTP not found. cannelloni will be build without SCTP support.")
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
  message(STATUS "Building cannelloni without mDNS discovery (AVAHI_SUPPORT=OFF)")
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
# the feature is enabled. The dependency propagates to the cannelloni executable.
if(AVAHI_SUPPORT)
    target_link_libraries(addsources PkgConfig::AVAHI)
endif(AVAHI_SUPPORT)
set_target_properties(addsources PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cannellonis addsources cannelloni-common-static pthread)
target_compile_features(cannellonis PRIVATE cxx_auto_type)
target_compile_features(addsources PRIVATE cxx_auto_type)

install(TARGETS cannellonis DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
install(TARGETS cannelloni-common DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT lib)

# ---------------------------------------------------------------------------
# Debian package: run `cpack -G DEB` from the build dir to produce a single
# cannellonis_<version>_<arch>.deb.
#
# Binary-only by design. The package ships just the cannellonis executable
# (which static-links cannelloni-common-static) plus docs. The shared
# libcannelloni-common is still installed by `cmake --install` for external
# consumers, but it lives in its own CPack component and is deliberately left
# out of the .deb: it is byte-for-byte upstream's libcannelloni-common0, so
# shipping it would only collide with upstream for no benefit.
#
# cannellonis is a fork of upstream `cannelloni` with the binary renamed, so
# there is no /usr/bin path clash. Breaks + Replaces declares that installing
# cannellonis supersedes an existing upstream `cannelloni` package (dpkg/apt
# removes it) rather than leaving two CAN tunnels installed side by side.
# ---------------------------------------------------------------------------
# DOCDIR derives from the project name (cannelloni); spell out cannellonis so
# the docs match the package name and don't share upstream's doc directory.
install(FILES gpl-2.0.txt README.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cannellonis
        COMPONENT runtime)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "cannellonis")
# Always emit the package into the build tree (which is git-ignored), even when
# cpack is run from the source root via --config build/CPackConfig.cmake.
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# Keep in sync with CANNELLONI_VERSION in cannelloni.cpp.
set(CPACK_PACKAGE_VERSION "2.0.1")
# Debian requires a Maintainer field; dpkg accepts a name without an email
# address (lintian only warns), so keep it email-free.
set(CPACK_PACKAGE_CONTACT "Soenke J. Peters")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "SocketCAN over Ethernet tunnel (multi-peer fork)")
set(CPACK_PACKAGE_DESCRIPTION
    "cannellonis tunnels SocketCAN frames over UDP, TCP or SCTP. It is a \
multi-peer fork of cannelloni: a single instance can bridge a CAN bus to many \
peers at once, forming a virtual shared bus, with optional mDNS/zeroconf peer \
discovery.")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/aalarchiv/cannelloni")
set(CPACK_DEBIAN_PACKAGE_BREAKS "cannelloni")
set(CPACK_DEBIAN_PACKAGE_REPLACES "cannelloni")
# Auto-compute Depends (libc6, libsctp1, libavahi-client3 ...) from the linked
# shared libraries rather than hand-maintaining them.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Package only the runtime component (binary + docs); the `lib` component
# (shared library) is excluded. Component-install mode is required here --
# in monolithic mode CPack runs every install() rule regardless of
# CPACK_COMPONENTS_ALL and the shared lib would leak back into the .deb.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime)
# Keep it a single deb literally named `cannellonis` (not cannellonis-runtime)
# with a Debian-style filename: cannellonis_<version>_<arch>.deb.
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "cannellonis")
set(CPACK_DEBIAN_RUNTIME_FILE_NAME "DEB-DEFAULT")
include(CPack)
