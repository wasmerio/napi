cmake_minimum_required(VERSION 3.20)
project(napi_engines LANGUAGES C CXX)

option(NAPI_BUILD_QUICKJS "Build the QuickJS-backed Node-API engine" ON)
option(NAPI_BUILD_V8 "Build the V8-backed Node-API engine" OFF)


if(NAPI_BUILD_QUICKJS)
  add_subdirectory(quickjs)
endif()

if(NAPI_BUILD_V8)
  add_subdirectory(v8)
endif()
