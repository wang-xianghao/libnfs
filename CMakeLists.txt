cmake_minimum_required(VERSION 3.2)

set(PICO $ENV{PICO})
set(PICO_EXTENSION $ENV{PICO_EXTENSION})
set(LIBGCC $ENV{LIBGCC})
# set(MUSL $ENV{MUSL})

set(CMAKE_SYSTEM_NAME Generic)

set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
set(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_C_FLAGS "-nostdinc -isystem=${MUSL}/newlib/libc/include")
# set(CMAKE_C_FLAGS "-nostdinc -isystem=${MUSL}/include")

project(libnfs
        LANGUAGES C
        VERSION 5.0.2)

set(SOVERSION 11.1.0 CACHE STRING "" FORCE)

set(INSTALL_BIN_DIR "${CMAKE_INSTALL_PREFIX}/bin" CACHE PATH "Installation directory for binaries")
set(INSTALL_LIB_DIR "${CMAKE_INSTALL_PREFIX}/lib" CACHE PATH "Installation directory for libraries")
set(INSTALL_INC_DIR "${CMAKE_INSTALL_PREFIX}/include" CACHE PATH "Installation directory for headers")
set(INSTALL_MAN_DIR "${CMAKE_INSTALL_PREFIX}/share/man" CACHE PATH "Installation directory for manual pages")
set(INSTALL_PKGCONFIG_DIR "${INSTALL_LIB_DIR}/pkgconfig" CACHE PATH "Installation directory for pkgconfig (.pc) files")
set(INSTALL_CMAKE_DIR "${INSTALL_LIB_DIR}/cmake/libnfs" CACHE PATH "Installation directory for cmake (.cmake) files")

set(HAVE_ARPA_INET_H 1)
set(HAVE_CLOCK_GETTIME 1)
set(HAVE_DLFCN_H 1)
set(HAVE_FUSE_H 1)
set(HAVE_INTTYPES_H 1)
set(HAVE_MEMORY_H 1)
set(HAVE_NETDB_H 1)
set(HAVE_NETINET_IN_H 1)
set(HAVE_NETINET_TCP_H 1)
set(HAVE_NET_IF_H 1)
set(HAVE_POLL_H 1)
set(HAVE_SOCKADDR_STORAGE 1)
set(HAVE_STDINT_H 1)
set(HAVE_STDLIB_H 1)
set(HAVE_STRINGS_H 1)
set(HAVE_STRINGS_H 1)
set(HAVE_STRING_H 1)
set(HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1)
set(HAVE_SYS_IOCTL_H 1)
set(HAVE_SYS_SOCKET_H 1)
set(HAVE_SYS_STATVFS_H 1)
set(HAVE_SYS_STAT_H 1)
set(HAVE_SYS_SYSMACROS_H 1)
set(HAVE_SYS_TIME_H 1)
set(HAVE_TIME_H 1)
set(HAVE_SYS_TYPES_H 1)
set(HAVE_SYS_VFS_H 1)
set(HAVE_TALLOC_TEVENT 1)
set(HAVE_UNISTD_H 1)
set(HAVE_UTIME_H 1)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(ENABLE_TESTS "Build and run test programs" OFF)
option(ENABLE_DOCUMENTATION "Build Documentation" OFF)
option(ENABLE_UTILS "Build util programs" OFF)
option(ENABLE_EXAMPLES "Build example programs" OFF)

if(ENABLE_TESTS)
  set(ENABLE_UTILS ON CACHE BOOL "Building utils required by tests" FORCE)
endif()

include(cmake/Macros.cmake)
include(cmake/ConfigureChecks.cmake)

include_directories(${CMAKE_CURRENT_BINARY_DIR}
                    include
                    include/nfsc
                    "${PICO}/newlib/libc/include"
                    "${PICO}/newlib/libc/include/sys"
                    "${PICO}/newlib/libc/sys"
                    "${PICO}/newlib/libc/stdlib"
                    "${PICO}/newlib/libc/signal"
                    "${PICO}/newlib/libc/locale"
                    "${PICO}/newlib/libc/tinystdio"
                    "${PICO_EXTENSION}/include"
                    "${PICO}"
                    "${LIBGCC}/include")
# include_directories(${CMAKE_CURRENT_BINARY_DIR}
#                     include
#                     include/nfsc
#                     "${MUSL}/include")

# list of system libs (sockets/etc) to be linked on this system
set(SYSTEM_LIBRARIES "${PICO}/lib/libc.a")
# list of core (static) libraries built in current configuration (used by lib/CMakeLists.txt)
set(CORE_LIBRARIES "" CACHE INTERNAL "")

# added microkit macro
add_definitions(-D__MICROKIT__)

# make sure related functions are exported in final dll
if(WIN32 AND BUILD_SHARED_LIBS)
  add_definitions(-Dlibnfs_EXPORTS)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  add_definitions("-D_U_=" -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE)
  list(APPEND SYSTEM_LIBRARIES ws2_32)
  add_subdirectory(win32)
elseif(CMAKE_SYSTEM_NAME STREQUAL Solaris)
  add_definitions("-D_U_=__attribute__((unused))")
  find_library(SOCKET_LIBRARY socket)
  find_library(NSL_LIBRARY nsl)
  list(APPEND SYSTEM_LIBRARIES ${SOCKET_LIBRARY} ${NSL_LIBRARY})
elseif(CMAKE_SYSTEM_NAME STREQUAL aros)
  add_definitions("-D_U_=__attribute__((unused))")
  add_definitions(-DAROS)
  add_subdirectory(aros)
else()
  add_definitions("-D_U_=__attribute__((unused))")
endif()

add_subdirectory(mount)
add_subdirectory(nfs)
add_subdirectory(nfs4)
add_subdirectory(nlm)
add_subdirectory(nsm)
add_subdirectory(portmap)
add_subdirectory(rquota)
add_subdirectory(lib)   # this has to be last (it links all static libs mentioned in CORE_LIBRARIES)

if(ENABLE_DOCUMENTATION)
  add_subdirectory(doc)
endif()

if(ENABLE_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ENABLE_UTILS)
  add_subdirectory(utils)
endif()

# this will create build-tree-specific config file (so downstream client can use this specific build without having to install package)
export(EXPORT libnfs
       NAMESPACE libnfs::
       FILE "${CMAKE_CURRENT_BINARY_DIR}/libnfs-config.cmake")

# this will create relocatable config file in installation directory
install(EXPORT libnfs
        DESTINATION "${INSTALL_CMAKE_DIR}"
        NAMESPACE libnfs::
        FILE libnfs-config.cmake)

# handle version file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libnfs-config-version.cmake
                                 VERSION ${PROJECT_VERSION}
                                 COMPATIBILITY AnyNewerVersion)

install(FILES cmake/FindNFS.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/libnfs-config-version.cmake
        DESTINATION ${INSTALL_CMAKE_DIR})

# handle pc-config files
configure_file(cmake/libnfs.pc.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/libnfs.pc @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libnfs.pc
        DESTINATION ${INSTALL_PKGCONFIG_DIR})

# handle headers installation
install(DIRECTORY include/nfsc
        DESTINATION ${INSTALL_INC_DIR})

install(FILES mount/libnfs-raw-mount.h
              nfs/libnfs-raw-nfs.h
              nfs4/libnfs-raw-nfs4.h
              nlm/libnfs-raw-nlm.h
              nsm/libnfs-raw-nsm.h
              portmap/libnfs-raw-portmap.h
              rquota/libnfs-raw-rquota.h
        DESTINATION ${INSTALL_INC_DIR}/nfsc)
