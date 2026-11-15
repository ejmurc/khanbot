cmake_minimum_required(VERSION 3.25)
project(khanbot
  VERSION 0.0.1
  LANGUAGES CXX
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
option(ENABLE_SANITIZERS "Enable sanitizers (ASan, UBSan)" OFF)
add_library(app_options INTERFACE)
if(MSVC)
  target_compile_options(app_options INTERFACE /W4 /WX /Zc:preprocessor)
  target_compile_options(app_options INTERFACE $<$<CONFIG:Release>:/O2>)
else()
  target_compile_options(app_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
  target_compile_options(app_options INTERFACE $<$<CONFIG:Release>:-O3>)
  target_compile_options(app_options INTERFACE $<$<CONFIG:Release>:-march=native>)
  target_compile_options(app_options INTERFACE $<$<CONFIG:Release>:-flto>)
  target_link_options(app_options INTERFACE $<$<CONFIG:Release>:-flto>)
  if(ENABLE_SANITIZERS)
    target_compile_options(app_options INTERFACE -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    target_link_options(app_options INTERFACE -fsanitize=address,undefined -fno-sanitize=vptr)
  endif()
endif()
find_package(Threads REQUIRED)
include(FetchContent)

set(CURL_IMPERSONATE_VERSION v2.3.2 CACHE STRING "libcurl-impersonate release tag")
set(CURL_IMPERSONATE_TAG "" CACHE STRING "Release asset platform tag; auto-detected when empty")
if(NOT CURL_IMPERSONATE_TAG)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(CURL_IMPERSONATE_TAG x86_64-linux-gnu)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CURL_IMPERSONATE_TAG aarch64-linux-gnu)
  elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(CURL_IMPERSONATE_TAG arm64-macos)
  elseif(APPLE)
    set(CURL_IMPERSONATE_TAG x86_64-macos)
  else()
    message(FATAL_ERROR "Unsupported platform; pass -DCURL_IMPERSONATE_TAG=<platform from the release assets>")
  endif()
endif()
FetchContent_Declare(
    curl_impersonate
    URL https://github.com/lexiforest/curl-impersonate/releases/download/${CURL_IMPERSONATE_VERSION}/libcurl-impersonate-${CURL_IMPERSONATE_VERSION}.${CURL_IMPERSONATE_TAG}.tar.gz
    SOURCE_SUBDIR do_not_add_subdirectory
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(curl_impersonate)
add_library(curl_impersonate_lib STATIC IMPORTED)
set_target_properties(curl_impersonate_lib PROPERTIES IMPORTED_LOCATION ${curl_impersonate_SOURCE_DIR}/libcurl-impersonate.a)
target_include_directories(curl_impersonate_lib SYSTEM INTERFACE ${curl_impersonate_SOURCE_DIR}/include)
target_link_libraries(curl_impersonate_lib INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(curl_impersonate_lib INTERFACE
    "-framework CoreFoundation" "-framework Security" "-framework CoreServices"
    resolv icucore iconv)
endif()

FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v9.0.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do_not_add_subdirectory
    SYSTEM
)
FetchContent_MakeAvailable(glaze)
add_library(glaze_headers INTERFACE)
target_include_directories(glaze_headers SYSTEM INTERFACE ${glaze_SOURCE_DIR}/include)

add_library(khanbot_core STATIC
  src/crawl.cc
  src/http.cc
  src/progress.cc
  src/resolve.cc
  src/watch.cc
)
target_include_directories(khanbot_core PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(khanbot_core PUBLIC glaze_headers curl_impersonate_lib Threads::Threads app_options)

add_executable(main src/main.cc)
target_link_libraries(main PRIVATE khanbot_core app_options)

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
  file(GLOB_RECURSE ALL_SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/src/*.cc
    ${CMAKE_SOURCE_DIR}/src/*.h
  )
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCE_FILES}
    VERBATIM
  )
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ALL_SOURCE_FILES}
    VERBATIM
  )
endif()
