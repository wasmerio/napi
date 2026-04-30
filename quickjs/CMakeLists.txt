include(FetchContent)

set(QUICKJS_NG_VERSION "v0.14.0")
set(QUICKJS_NG_URL
  "https://github.com/quickjs-ng/quickjs/archive/refs/tags/${QUICKJS_NG_VERSION}.tar.gz"
)

FetchContent_Declare(quickjs_ng_sources
  URL "${QUICKJS_NG_URL}"
  URL_HASH SHA256=928e9406addd99eb8623348f2cfcd916eade9a263c60d42be79bc7aee4ee8453
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR edge-cmake-noop
)
FetchContent_MakeAvailable(quickjs_ng_sources)
include("${CMAKE_CURRENT_SOURCE_DIR}/patch_quickjs_ng.cmake")

add_library(quickjs_ng STATIC
  "${quickjs_ng_sources_SOURCE_DIR}/dtoa.c"
  "${quickjs_ng_sources_SOURCE_DIR}/libregexp.c"
  "${quickjs_ng_sources_SOURCE_DIR}/libunicode.c"
  "${quickjs_ng_sources_SOURCE_DIR}/quickjs.c"
)

target_include_directories(quickjs_ng PUBLIC "${quickjs_ng_sources_SOURCE_DIR}")
target_compile_definitions(quickjs_ng PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
target_compile_features(quickjs_ng PUBLIC c_std_11)

find_library(QUICKJS_NG_M_LIBRARY m)
if(QUICKJS_NG_M_LIBRARY)
  target_link_libraries(quickjs_ng PUBLIC "${QUICKJS_NG_M_LIBRARY}")
endif()

add_library(napi_quickjs STATIC
  napi_quickjs_env.cc
  napi_quickjs_unofficial.cc
  napi_quickjs_values.cc
)

target_include_directories(napi_quickjs
  PUBLIC
    "${NAPI_INCLUDE_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/internal"
)

target_compile_features(napi_quickjs PUBLIC cxx_std_20)
target_link_libraries(napi_quickjs PUBLIC quickjs_ng)
target_compile_definitions(napi_quickjs
  PUBLIC
    NAPI_EXTERN=
    NAPI_EXTENSION_WASMER_EXTERN=
    EDGE_EMBEDDED_NAPI_PROVIDER=1
)

message(STATUS "EDGE: QuickJS N-API provider enabled with QuickJS-NG ${QUICKJS_NG_VERSION}")
