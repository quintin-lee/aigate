# Reformat the project sources with clang-format as part of the build.
#
#   cmake -B build -DAIGATE_FORMAT_ON_BUILD=OFF      # opt out
#   cmake -B build -DFORMAT_CLANG_FORMAT=/usr/bin/clang-format-18
#
# All project sources are handled by a single `format` target that every
# project target depends on, so the files are rewritten before the first object
# is compiled and two invocations can never race on the same file.
# Third-party, generated and build-tree files are never touched.

option(AIGATE_FORMAT_ON_BUILD "Reformat the project sources with clang-format on every build" ON)

# Make the given targets wait for the reformat pass. Defined unconditionally so
# callers do not have to care whether formatting is enabled.
function(aigate_clang_format_on_build)
    foreach(_target IN LISTS ARGN)
        if(TARGET ${_target} AND TARGET format)
            add_dependencies(${_target} format)
        endif()
    endforeach()
endfunction()

if(NOT AIGATE_FORMAT_ON_BUILD)
    return()
endif()

find_program(
    FORMAT_CLANG_FORMAT
    NAMES clang-format clang-format-22 clang-format-21 clang-format-20 clang-format-19
          clang-format-18 clang-format-17 clang-format-16 clang-format-15
    DOC "clang-format executable used to reformat the sources on every build")

if(NOT FORMAT_CLANG_FORMAT)
    message(WARNING "AIGATE_FORMAT_ON_BUILD is ON but no clang-format was found; "
                    "sources are left untouched. Install clang-format or pass "
                    "-DFORMAT_CLANG_FORMAT=<path>.")
    return()
endif()

file(
    GLOB_RECURSE AIGATE_FORMAT_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.c"
    "${CMAKE_SOURCE_DIR}/tests/*.h")
list(FILTER AIGATE_FORMAT_FILES EXCLUDE REGEX "/(third_party|_deps|generated)/")

if(NOT AIGATE_FORMAT_FILES)
    return()
endif()

list(LENGTH AIGATE_FORMAT_FILES AIGATE_FORMAT_FILE_COUNT)

add_custom_target(
    format ALL
    COMMAND
        python3 ${CMAKE_SOURCE_DIR}/scripts/clang_format.py --clang-format
        ${FORMAT_CLANG_FORMAT} ${AIGATE_FORMAT_FILES}
    COMMENT "clang-format ${AIGATE_FORMAT_FILE_COUNT} file(s)"
    VERBATIM)
