cmake_minimum_required(VERSION 3.16)
project(aigate C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)
pkg_check_modules(JANSSON REQUIRED jansson)

include(FetchContent)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/hdr_histogram-0.11.6.tar.gz")
    FetchContent_Declare(hdr_hist_dep URL "${CMAKE_SOURCE_DIR}/third_party/hdr_histogram-0.11.6.tar.gz")
else()
    FetchContent_Declare(hdr_hist_dep URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.6.tar.gz)
endif()
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hdr_hist_dep)

set(CIVETWEB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
# CivetWeb ships sanitizers baked into its Debug builds; that would leak
# -fsanitize=undefined into the parent link line for the main executable.
# The main executable links against the civetweb library, so any -fsanitize
# flags must be present in the parent link line as well, or link fails with
# undefined __ubsan_* / __asan_* symbols.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CIVETWEB_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
endif()
set(CIVETWEB_THREAD_STACK_SIZE 1048576 CACHE STRING "Stack size for threads" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/civetweb-1.16.tar.gz")
    FetchContent_Declare(civetweb_dep URL "${CMAKE_SOURCE_DIR}/third_party/civetweb-1.16.tar.gz")
else()
    FetchContent_Declare(civetweb_dep URL https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz)
endif()
FetchContent_MakeAvailable(civetweb_dep)

# Generate embedded admin UI header
set(ADMIN_UI_HTML_SRC ${CMAKE_SOURCE_DIR}/web/admin.html)
set(ADMIN_UI_HTML_HDR ${CMAKE_BINARY_DIR}/generated/admin_ui_html.h)
add_custom_command(
    OUTPUT ${ADMIN_UI_HTML_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_html.py ${ADMIN_UI_HTML_SRC} ${ADMIN_UI_HTML_HDR}
    DEPENDS ${ADMIN_UI_HTML_SRC} ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
    COMMENT "Generating ${ADMIN_UI_HTML_HDR} from ${ADMIN_UI_HTML_SRC}"
)
add_custom_target(generate_admin_ui_html DEPENDS ${ADMIN_UI_HTML_HDR})

file(GLOB AIGATE_SRC src/*.c)
list(REMOVE_ITEM AIGATE_SRC ${CMAKE_SOURCE_DIR}/src/main.c)
# Optional ASan/UBSan build: -DAIGATE_SANITIZERS=ON adds
# -fsanitize=address,undefined + -O1 -g to aigate + libaigate.
option(AIGATE_SANITIZERS "Build aigate with AddressSanitizer/UBSan" OFF)
if(AIGATE_SANITIZERS)
    set(AIGATE_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
endif()

add_library(libaigate STATIC ${AIGATE_SRC})
add_dependencies(libaigate generate_admin_ui_html civetweb-c-library hdr_histogram_static)
target_include_directories(libaigate PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_compile_options(libaigate PRIVATE -Wall -Wextra -Werror)
if(AIGATE_SANITIZERS)
    target_compile_options(libaigate PRIVATE ${AIGATE_SAN_FLAGS})
    target_link_libraries(libaigate PUBLIC ${AIGATE_SAN_FLAGS})
endif()
target_include_directories(libaigate PUBLIC ${JANSSON_INCLUDE_DIRS} ${LIBPQ_INCLUDE_DIRS})
target_link_libraries(libaigate PUBLIC
  ${JANSSON_LIBRARIES}
  hdr_histogram_static
  civetweb-c-library
  CURL::libcurl ${LIBPQ_LIBRARIES} OpenSSL::Crypto ${CMAKE_THREAD_LIBS_INIT})

add_executable(aigate src/main.c)
target_compile_options(aigate PRIVATE -Wall -Wextra -Werror)
if(AIGATE_SANITIZERS)
    target_compile_options(aigate PRIVATE ${AIGATE_SAN_FLAGS})
    target_link_libraries(aigate PRIVATE ${AIGATE_SAN_FLAGS})
endif()
target_link_libraries(aigate PRIVATE libaigate civetweb-c-library)


enable_testing()
include(CTest)
add_subdirectory(tests)
