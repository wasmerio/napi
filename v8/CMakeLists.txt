cmake_minimum_required(VERSION 3.20)
project(napi_v8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

macro(xoption OPTION_NAME OPTION_TEXT OPTION_DEFAULT)
  option(${OPTION_NAME} ${OPTION_TEXT} ${OPTION_DEFAULT})
  if(DEFINED ENV{${OPTION_NAME}})
    # Allow setting the option through an environment variable.
    set(${OPTION_NAME} $ENV{${OPTION_NAME}})
  endif()
  if(${OPTION_NAME})
    add_definitions(-D${OPTION_NAME})
  endif()
  message(STATUS "  ${OPTION_NAME}: ${${OPTION_NAME}}")
endmacro()

xoption(NAPI_V8_BUILD_TESTS "Build napi-v8 tier1 tests" ON)
xoption(NAPI_V8_ENABLE_LIFETIME_TRACKER
  "Enable V8 N-API lifetime tracking diagnostics"
  OFF
)
xoption(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS
  "Print V8 N-API value/ref allocator stats every 10 seconds"
  OFF
)
xoption(NAPI_V8_ENABLE_LIFETIME_TAG_STATS
  "Print V8 N-API active value/ref tag stats with periodic stats"
  OFF
)
xoption(NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP
  "Print active V8 N-API string and symbol values with periodic stats"
  OFF
)

if(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS AND NOT NAPI_V8_ENABLE_LIFETIME_TRACKER)
  message(STATUS
    "NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS requires "
    "NAPI_V8_ENABLE_LIFETIME_TRACKER; enabling lifetime tracker"
  )
  set(NAPI_V8_ENABLE_LIFETIME_TRACKER ON)
endif()
if(NAPI_V8_ENABLE_LIFETIME_TAG_STATS AND NOT NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS)
  message(STATUS
    "NAPI_V8_ENABLE_LIFETIME_TAG_STATS requires "
    "NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS; enabling periodic stats"
  )
  set(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS ON)
endif()
if(NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP AND NOT NAPI_V8_ENABLE_LIFETIME_TAG_STATS)
  message(STATUS
    "NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP requires "
    "NAPI_V8_ENABLE_LIFETIME_TAG_STATS; enabling tag stats"
  )
  set(NAPI_V8_ENABLE_LIFETIME_TAG_STATS ON)
endif()
if(NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS AND NOT NAPI_V8_ENABLE_LIFETIME_TRACKER)
  set(NAPI_V8_ENABLE_LIFETIME_TRACKER ON)
endif()

set(NAPI_V8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${NAPI_V8_ROOT}/cmake/NapiBuildCache.cmake")
napi_enable_sccache_if_available()
include("${NAPI_V8_ROOT}/cmake/NapiEngine.cmake")
include("${NAPI_V8_ROOT}/cmake/NapiV8Resolver.cmake")
napi_engine_set_layout("${NAPI_V8_ROOT}")
set(NAPI_V8_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
if(DEFINED ENV{NAPI_V8_NODE_ROOT})
  set(NAPI_V8_NODE_ROOT_DEFAULT "$ENV{NAPI_V8_NODE_ROOT}")
endif()
set(NAPI_V8_NODE_ROOT
  "${NAPI_V8_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
)
napi_v8_resolve_configuration()

file(GLOB_RECURSE NAPI_V8_SRC_FILES
  "${NAPI_V8_ROOT}/src/*.h"
  "${NAPI_V8_ROOT}/src/*.cc"
  "${NAPI_V8_ROOT}/src/*.c"
)
foreach(NAPI_V8_SRC_FILE IN LISTS NAPI_V8_SRC_FILES)
  file(READ "${NAPI_V8_SRC_FILE}" NAPI_V8_SRC_CONTENTS)
  if(NAPI_V8_SRC_CONTENTS MATCHES "#include <uv\\.h>" OR
     NAPI_V8_SRC_CONTENTS MATCHES "#include \"uv\\.h\"")
    message(FATAL_ERROR
      "libuv usage is forbidden in napi/v8/src. "
      "Found in: ${NAPI_V8_SRC_FILE}")
  endif()
endforeach()

add_library(napi_v8 STATIC
  ../lib/napi_lifetime_tracker.cc
  src/js_native_api_v8.cc
  src/internal/napi_escapable_handle_scope_wrapper.cc
  src/internal/napi_external_wrapper.cc
  src/internal/napi_function_callback_info.cc
  src/internal/napi_getter_callback_info.cc
  src/internal/napi_handle_scope_wrapper.cc
  src/internal/napi_lifetime_tracker.cc
  src/internal/napi_ref.cc
  src/internal/napi_ref_tracker.cc
  src/internal/napi_ref_with_data.cc
  src/internal/napi_ref_with_finalizer.cc
  src/internal/napi_setter_callback_info.cc
  src/internal/napi_util.cc
  src/unofficial_napi.cc
  src/unofficial_napi_error_utils.cc
  src/unofficial_napi_contextify.cc
  src/edge_v8_platform.cc
)

target_include_directories(napi_v8
  PUBLIC
    "${NAPI_INCLUDE_ROOT}"
    "${PROJECT_ROOT}/napi/lib"
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_INCLUDE_DIR}"
)

target_compile_definitions(napi_v8
  PUBLIC
    NAPI_EXTERN=
    EDGE_BUNDLED_NAPI_V8=1
  PRIVATE
    $<$<BOOL:${NAPI_V8_ENABLE_LIFETIME_TRACKER}>:NAPI_V8_ENABLE_LIFETIME_TRACKER=1>
    $<$<BOOL:${NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS}>:NAPI_V8_ENABLE_LIFETIME_PERIODIC_STATS=1>
    $<$<BOOL:${NAPI_V8_ENABLE_LIFETIME_TAG_STATS}>:NAPI_V8_ENABLE_LIFETIME_TAG_STATS=1>
    $<$<BOOL:${NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP}>:NAPI_V8_ENABLE_LIFETIME_STRING_SYMBOL_DUMP=1>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(napi_v8 PRIVATE -fno-rtti)
endif()
if(NAPI_V8_DEFINES)
  target_compile_definitions(napi_v8 PRIVATE ${NAPI_V8_DEFINES})
endif()
if(NAPI_V8_LIBRARY)
  target_link_libraries(napi_v8 PUBLIC "${NAPI_V8_LIBRARY}")
endif()
if(NAPI_V8_EXTRA_LIBS)
  target_link_libraries(napi_v8 PUBLIC ${NAPI_V8_EXTRA_LIBS})
endif()
if(NAPI_V8_BUILD_TESTS)
  enable_testing()
  set(NAPI_TEST_ENGINE v8)
  add_subdirectory("${NAPI_TESTS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
