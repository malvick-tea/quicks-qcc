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
qcc_add_test(intern)
qcc_add_test(token)
qcc_add_test(lexer)
qcc_add_test(hideset)
qcc_add_test(macro)
qcc_add_test(pp)

# The #include suite needs real header files. Generate them into the build tree
# so the test is hermetic: it depends on these fixtures and their directories
# (passed in as macros), never on the working directory or the repo layout.
set(qcc_incfix ${CMAKE_CURRENT_BINARY_DIR}/incfix)
file(WRITE ${qcc_incfix}/angle/only_angle.h  "int from_angle;\n")
file(WRITE ${qcc_incfix}/angle/common.h      "int common_angle;\n")
file(WRITE ${qcc_incfix}/angle/chain1.h      "#include <chain2.h>\nint chain1;\n")
file(WRITE ${qcc_incfix}/angle/chain2.h      "int chain2;\n")
file(WRITE ${qcc_incfix}/angle/sub/deep.h    "int deep;\n")
file(WRITE ${qcc_incfix}/angle/filewithloc.h "__FILE__\n__LINE__\n")
file(WRITE ${qcc_incfix}/angle/selfinc.h     "#include <selfinc.h>\n")
file(WRITE ${qcc_incfix}/quote/only_quote.h  "int from_quote;\n")

add_executable(test_include test_include.c)
target_link_libraries(test_include PRIVATE qcc_core)
target_include_directories(test_include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_include PRIVATE
    QCC_INC_ANGLE_DIR="${qcc_incfix}/angle"
    QCC_INC_QUOTE_DIR="${qcc_incfix}/quote")
add_test(NAME include COMMAND test_include)
