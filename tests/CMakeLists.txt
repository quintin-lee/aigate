file(GLOB UNIT_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/*.c ${CMAKE_SOURCE_DIR}/tests/unit/*/*.c)
add_executable(aigate_unit_tests ${UNIT_TEST_SRC})
target_include_directories(aigate_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
target_link_libraries(aigate_unit_tests PRIVATE libaigate civetweb-c-library OpenSSL::Crypto)
add_dependencies(aigate_unit_tests civetweb-c-library)
add_test(NAME unit COMMAND aigate_unit_tests)

aigate_clang_format_on_build(aigate_unit_tests)
