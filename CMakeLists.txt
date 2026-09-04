cmake_minimum_required(VERSION 3.16)
project(uae_portable_cpu C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(ENABLE_TESTS "Build tests" ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/softfloat
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Generator tools
add_executable(build68k
    src/cpu/build68k.c
    src/cpu/writelog.c
)
target_include_directories(build68k PRIVATE src/cpu)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cpudefs.c
    COMMAND $<TARGET_FILE:build68k> < ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/table68k > ${CMAKE_CURRENT_BINARY_DIR}/cpudefs.c
    DEPENDS build68k ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/table68k
    COMMENT "Generating cpudefs.c from table68k"
)

add_executable(gencpu
    src/cpu/gencpu.c
    src/cpu/readcpu.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpudefs.c
)
target_include_directories(gencpu PRIVATE src/cpu ${CMAKE_CURRENT_BINARY_DIR})

set(CPUEMU_SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/cpustbl.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_0.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_11.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_13.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_20.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_21.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_22.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_23.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_24.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_31.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_32.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_33.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_34.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_35.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_40.c
    ${CMAKE_CURRENT_BINARY_DIR}/cpuemu_50.c
)

add_custom_command(
    OUTPUT ${CPUEMU_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/cputbl.h
    COMMAND $<TARGET_FILE:gencpu>
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS gencpu
    COMMENT "Generating 680x0 CPU opcode tables and emulation routines"
)

# Core library sources
set(CORE_SOURCES
    ${CPUEMU_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/cpudefs.c
    src/cpu/newcpu.c
    src/cpu/newcpu_common.c
    src/cpu/cpummu.c
    src/cpu/cpummu030.c
    src/cpu/fpp.c
    src/cpu/fpp_softfloat.c
    src/cpu/fpp_native.c
    src/cpu/disasm.c
    src/cpu/events.c
    src/cpu/memory.c
    src/cpu/uae_glue.c
    src/cpu/readcpu.c
    src/cpu/writelog.c
    src/cpu/softfloat/softfloat.c
    src/cpu/softfloat/softfloat_decimal.c
    src/cpu/softfloat/softfloat_fpsp.c
    src/api/musashi_api.c
    src/api/uae_cpu_api.c
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set_source_files_properties(${CPUEMU_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-sign-compare -Wno-unused-variable -Wno-unused-function -Wno-parentheses -fwrapv")
    set_source_files_properties(src/cpu/newcpu.c src/cpu/newcpu_common.c src/cpu/cpummu.c src/cpu/cpummu030.c PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-unused-function -fwrapv")
endif()

add_library(uaecpu STATIC ${CORE_SOURCES})
target_include_directories(uaecpu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/softfloat
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(ENABLE_TESTS)
    enable_testing()

    add_executable(test_basic tests/test_basic.c)
    target_link_libraries(test_basic PRIVATE uaecpu m)

    add_executable(test_musashi_suite tests/test_musashi_suite.c)
    target_link_libraries(test_musashi_suite PRIVATE uaecpu m)

    add_executable(test_m68k_rs_suite tests/test_m68k_rs_suite.c)
    target_link_libraries(test_m68k_rs_suite PRIVATE uaecpu m)

    add_executable(test_uae_cpu tests/test_uae_cpu.c)
    target_link_libraries(test_uae_cpu PRIVATE uaecpu m)

    add_test(NAME test_basic COMMAND test_basic)
    add_test(NAME test_uae_cpu COMMAND test_uae_cpu)
    add_test(NAME musashi_68000 COMMAND test_musashi_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/musashi/mc68000 68000)
    add_test(NAME musashi_68040 COMMAND test_musashi_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/musashi/mc68040 68040)
    add_test(NAME m68k_rs_coverage COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra/coverage/bin 68040)
    add_test(NAME m68k_rs_extra COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra 68040)
endif()
