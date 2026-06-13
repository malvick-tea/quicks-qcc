# qcc — unit tests.
#
# One executable per suite, each linking qcc_core and registered with CTest. A
# helper function keeps this DRY: there is no per-test boilerplate to drift, and
# adding a suite is a single line (no hardcoded repetition — coding-standard.md).

function(qcc_add_test name)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE qcc_core)
    # The test harness header (qtest.h) lives alongside the test sources.
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

qcc_add_test(status)
qcc_add_test(source)
qcc_add_test(diag)
qcc_add_test(arena)
qcc_add_test(token)
qcc_add_test(lexer)
