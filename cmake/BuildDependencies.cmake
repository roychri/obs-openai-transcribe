cmake_minimum_required(VERSION 3.14)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/build_conan)

# Boost.Beast (header-only) provides the WebSocket client; OpenSSL backs its TLS stream.
find_package(Boost CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(ZLIB CONFIG REQUIRED PATHS ${CMAKE_SOURCE_DIR}/build_conan NO_DEFAULT_PATH)

list(
  APPEND
  DEPS_LIBRARIES
  openssl::openssl
  ${openssl_LIBS_RELEASE}
  ${zlib_LIBS_RELEASE})

if(WIN32)
  list(
    APPEND
    DEPS_LIBRARIES
    ws2_32
    crypt32
    bcrypt
    advapi32
    user32
    shlwapi)
endif()
if(APPLE)
  list(APPEND DEPS_LIBRARIES resolv)
endif()

list(
  APPEND
  DEPS_LIB_DIRS
  ${openssl_LIB_DIRS_RELEASE}
  ${zlib_LIB_DIRS_RELEASE}
  ${boost_LIB_DIRS_RELEASE})

list(
  APPEND
  DEPS_INCLUDE_DIRS
  ${Boost_INCLUDE_DIRS}
  ${openssl_INCLUDE_DIRS_RELEASE}
  ${zlib_INCLUDE_DIRS_RELEASE})

message(STATUS "Dependencies include directories: ${DEPS_INCLUDE_DIRS}")
message(STATUS "Dependencies library directories: ${DEPS_LIB_DIRS}")
message(STATUS "Dependencies libraries: ${DEPS_LIBRARIES}")

# Add include directories
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${DEPS_INCLUDE_DIRS})

# Link libraries
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${DEPS_LIB_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${DEPS_LIBRARIES})

if(MSVC)
  # Conan's OpenSSL ships no PDBs, so the linker emits LNK4099 for every static lib
  # object. CI links with /WX, which turns that into LNK1218 and produces no binary.
  # Silence just this warning rather than dropping warnings-as-errors for our own code.
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE /IGNORE:4099)
endif()
