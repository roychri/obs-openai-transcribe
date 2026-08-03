include(FetchContent)

set(LibCurl_VERSION "8.4.1")
set(LibCurl_BASEURL "https://github.com/occ-ai/obs-ai-libcurl-dep/releases/download/${LibCurl_VERSION}")

# CMAKE_BUILD_TYPE is empty under multi-config generators (Visual Studio), which the
# current plugintemplate uses on Windows. Dereferencing it there yields a malformed
# if(), so compare the variable by name and default an unset build type to Release.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL
                                                                 "RelWithDebInfo")
  set(LibCurl_BUILD_TYPE Release)
else()
  set(LibCurl_BUILD_TYPE Debug)
endif()

if(APPLE)
  if(LibCurl_BUILD_TYPE STREQUAL Release)
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-macos-${LibCurl_VERSION}-Release.tar.gz")
    set(LibCurl_HASH SHA256=700dc8ba476978bf8ee60c92fe31f7e1e31b7d67a47452f1d78b38ac7afd8962)
  else()
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-macos-${LibCurl_VERSION}-Debug.tar.gz")
    set(LibCurl_HASH SHA256=ee014693c74bb33d1851e2f136031cf4c490b7400c981a204a61cd5d3afd268a)
  endif()
elseif(MSVC)
  if(LibCurl_BUILD_TYPE STREQUAL Release)
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-windows-${LibCurl_VERSION}-Release.zip")
    set(LibCurl_HASH SHA256=7b40e4c1b80f1ade3051fb30077ff9dec6ace5cb0f46ba2ec35b35fdcafef5ff)
  else()
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-windows-${LibCurl_VERSION}-Debug.zip")
    set(LibCurl_HASH SHA256=d972ff7d473f43172f9ad8b9ad32015c4c85621b84d099d748278e0920c60a64)
  endif()
else()
  if(LibCurl_BUILD_TYPE STREQUAL Release)
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-linux-${LibCurl_VERSION}-Release.tar.gz")
    set(LibCurl_HASH SHA256=3e4769575682b84bb916f55411eac0541c78199087e127b499f9c18c8afd7203)
  else()
    set(LibCurl_URL "${LibCurl_BASEURL}/libcurl-linux-${LibCurl_VERSION}-Debug.tar.gz")
    set(LibCurl_HASH SHA256=fe0c1164c6b19def6867f0cbae979bb8a165e04ebb02cde6eb9b8af243a34483)
  endif()
endif()

FetchContent_Declare(
  libcurl_fetch
  URL ${LibCurl_URL}
  URL_HASH ${LibCurl_HASH})
FetchContent_MakeAvailable(libcurl_fetch)

if(MSVC)
  set(libcurl_fetch_lib_location "${libcurl_fetch_SOURCE_DIR}/lib/libcurl.lib")
  set(libcurl_fetch_link_libs "\$<LINK_ONLY:ws2_32>;\$<LINK_ONLY:advapi32>;\$<LINK_ONLY:crypt32>;\$<LINK_ONLY:bcrypt>")
else()
  find_package(ZLIB REQUIRED)
  set(libcurl_fetch_lib_location "${libcurl_fetch_SOURCE_DIR}/lib/libcurl.a")
  if(UNIX AND NOT APPLE)
    find_package(OpenSSL REQUIRED)
    set(libcurl_fetch_link_libs "\$<LINK_ONLY:OpenSSL::SSL>;\$<LINK_ONLY:OpenSSL::Crypto>;\$<LINK_ONLY:ZLIB::ZLIB>")
  else()
    set(libcurl_fetch_link_libs
        "-framework SystemConfiguration;-framework Security;-framework CoreFoundation;-framework CoreServices;ZLIB::ZLIB"
    )
  endif()
endif()

# Create imported target
add_library(libcurl STATIC IMPORTED)

set_target_properties(
  libcurl
  PROPERTIES INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
             INTERFACE_INCLUDE_DIRECTORIES "${libcurl_fetch_SOURCE_DIR}/include"
             INTERFACE_LINK_LIBRARIES "${libcurl_fetch_link_libs}")
set_property(
  TARGET libcurl
  APPEND
  PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libcurl PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C" IMPORTED_LOCATION_RELEASE
                                                                                       ${libcurl_fetch_lib_location})
