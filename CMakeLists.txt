cmake_minimum_required(VERSION 3.16)
project(aigate C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)
pkg_check_modules(JANSSON REQUIRED jansson)

include(FetchContent)
FetchContent_Declare(hdr_hist_dep URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.6.tar.gz)
FetchContent_MakeAvailable(hdr_hist_dep)
set(CIVETWEB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(civetweb_dep URL https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz)
FetchContent_MakeAvailable(civetweb_dep)

file(GLOB AIGATE_SRC src/*.c)
list(REMOVE_ITEM AIGATE_SRC ${CMAKE_SOURCE_DIR}/src/main.c)
add_library(libaigate STATIC ${AIGATE_SRC})
add_dependencies(libaigate civetweb-c-library hdr_histogram_static)
target_include_directories(libaigate PUBLIC src)
target_compile_options(libaigate PRIVATE -Wall -Wextra -Werror)
target_include_directories(libaigate PUBLIC ${JANSSON_INCLUDE_DIRS} ${LIBPQ_INCLUDE_DIRS})
target_link_libraries(libaigate PUBLIC
  ${JANSSON_LIBRARIES}
  hdr_histogram_static
  CURL::libcurl ${LIBPQ_LIBRARIES} OpenSSL::Crypto ${CMAKE_THREAD_LIBS_INIT})

add_executable(aigate src/main.c)
target_compile_options(aigate PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aigate PRIVATE libaigate civetweb-c-library)


enable_testing()
include(CTest)
add_subdirectory(tests)
