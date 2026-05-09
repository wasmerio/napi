cmake_minimum_required(VERSION 3.20)
project(napi_quickjs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAPI_QUICKJS_BUILD_TESTS "Build napi-quickjs tier1 tests" ON)

set(NAPI_QUICKJS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# include("${NAPI_V8_ROOT}/cmake/NapiBuildCache.cmake")
# napi_enable_sccache_if_available()
# include("${NAPI_V8_ROOT}/cmake/NapiEngine.cmake")
# include("${NAPI_V8_ROOT}/cmake/NapiV8Resolver.cmake")
# napi_engine_set_layout("${NAPI_V8_ROOT}")
# set(NAPI_V8_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
# if(DEFINED ENV{NAPI_V8_NODE_ROOT})
#   set(NAPI_V8_NODE_ROOT_DEFAULT "$ENV{NAPI_V8_NODE_ROOT}")
# endif()
# set(NAPI_V8_NODE_ROOT
#   "${NAPI_V8_NODE_ROOT_DEFAULT}"
#   CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
# )
# napi_v8_resolve_configuration()

include("${NAPI_QUICKJS_ROOT}/cmake/NapiEngine.cmake")
napi_engine_set_layout("${NAPI_QUICKJS_ROOT}")

set(NAPI_QUICKJS_NODE_ROOT_DEFAULT "${NAPI_QUICKJS_ROOT}/src/quickjs")
set(NAPI_QUICKJS_INCLUDE_DIR
  "${NAPI_QUICKJS_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to QuickJS headers"
)
set(NAPI_QUICKJS_LIBRARY_DEFAULT "${PROJECT_ROOT}/build-quickjs-napi/quickjs/libqjs.a")
set(NAPI_QUICKJS_LIBRARY
  "${NAPI_QUICKJS_LIBRARY_DEFAULT}"
  CACHE FILEPATH "Path to QuickJS static library for standalone builds"
)

message(STATUS "Resolved QuickJS include dir: ${NAPI_QUICKJS_INCLUDE_DIR}")
if(NOT TARGET qjs)
  message(STATUS "Resolved QuickJS library: ${NAPI_QUICKJS_LIBRARY}")
endif()

file(GLOB_RECURSE NAPI_QUICKJS_SRC_FILES
  "${NAPI_QUICKJS_ROOT}/src/*.h"
  "${NAPI_QUICKJS_ROOT}/src/*.cc"
  "${NAPI_QUICKJS_ROOT}/src/*.c"
)

# foreach(NAPI_V8_SRC_FILE IN LISTS NAPI_V8_SRC_FILES)
#   file(READ "${NAPI_V8_SRC_FILE}" NAPI_V8_SRC_CONTENTS)
#   if(NAPI_V8_SRC_CONTENTS MATCHES "#include <uv\\.h>" OR
#      NAPI_V8_SRC_CONTENTS MATCHES "#include \"uv\\.h\"")
#     message(FATAL_ERROR
#       "libuv usage is forbidden in napi/v8/src. "
#       "Found in: ${NAPI_V8_SRC_FILE}")
#   endif()
# endforeach()

add_library(napi_quickjs STATIC
  src/js_native_api_quickjs.cc
  src/internal/napi_callback_info.cc
  src/internal/napi_contextify.cc
  src/internal/napi_deferred.cc
  src/internal/napi_env.cc
  src/internal/napi_env_cleanup_hook.cc
  src/internal/napi_escapable_handle_scope.cc
  src/internal/napi_external.cc
  src/internal/napi_external_backing_store_hint.cc
  src/internal/napi_function.cc
  src/internal/napi_handle_scope.cc
  src/internal/napi_promises.cc
  src/internal/napi_ref.cc
  src/internal/napi_scope.cc
  src/internal/napi_serdes.cc
  src/internal/napi_set_property.cc
  src/internal/napi_util.cc
  src/internal/napi_value.cc
  src/unofficial_napi.cc
)

target_include_directories(napi_quickjs
  PUBLIC
    "${NAPI_INCLUDE_ROOT}"
    "${NAPI_QUICKJS_ROOT}/src"
  PRIVATE
    "${NAPI_QUICKJS_INCLUDE_DIR}"
)

target_compile_definitions(napi_quickjs
  PUBLIC
    NAPI_EXTERN=
    NAPI_BUNDLED_QUICKJS=1
  PRIVATE
    NAPI_QUICKJS_ENABLE_TRACE_DIAGNOSTICS=$<IF:$<CONFIG:Debug>,1,0>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(napi_quickjs PRIVATE -fno-rtti)
endif()
if(NAPI_QUICKJS_DEFINES)
  target_compile_definitions(napi_quickjs PRIVATE ${NAPI_QUICKJS_DEFINES})
endif()
if(TARGET qjs)
  target_link_libraries(napi_quickjs PUBLIC qjs)
elseif(NAPI_QUICKJS_LIBRARY)
  target_link_libraries(napi_quickjs PUBLIC "${NAPI_QUICKJS_LIBRARY}")
endif()
if(NAPI_QUICKJS_EXTRA_LIBS)
  target_link_libraries(napi_quickjs PUBLIC ${NAPI_QUICKJS_EXTRA_LIBS})
endif()
if(NAPI_QUICKJS_BUILD_TESTS)
  enable_testing()
  set(NAPI_TEST_ENGINE quickjs)
  add_subdirectory("${NAPI_TESTS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
