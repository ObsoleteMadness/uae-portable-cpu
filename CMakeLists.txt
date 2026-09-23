cmake_minimum_required(VERSION 3.16)
project(uae_portable_cpu VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(ENABLE_TESTS "Build tests" ON)
option(UAE_CPU_MUSASHI_API "Build the Musashi-compatible m68k_* API (include/m68k.h)" ON)
option(UAE_CPU_ISOLATE_SYMBOLS "Also build uaecpu_isolated, a static library exporting only the public API" OFF)
# lld-link cannot link relocatably, so Windows clang isolates by renaming instead (see below)
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    set(UAE_CPU_ISOLATE_BY_RENAME_DEFAULT ON)
else()
    set(UAE_CPU_ISOLATE_BY_RENAME_DEFAULT OFF)
endif()
option(UAE_CPU_ISOLATE_BY_RENAME "Isolate symbols by renaming the archive's internals instead of a relocatable link" ${UAE_CPU_ISOLATE_BY_RENAME_DEFAULT})
set(UAE_CPU_JIT "AUTO" CACHE STRING "Build the 680x0 JIT compiler: AUTO (on for supported hosts), ON or OFF")
set_property(CACHE UAE_CPU_JIT PROPERTY STRINGS AUTO ON OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
    src/cpu/host_hooks.c
    src/api/uae_cpu_api.c
)
if(UAE_CPU_MUSASHI_API)
    list(APPEND CORE_SOURCES src/api/musashi_api.c)
endif()

# JIT: AArch64 and x86-64 backends from WinUAE / ARAnyM / Amiberry.
if(CMAKE_OSX_ARCHITECTURES)
    set(UAE_CPU_JIT_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(UAE_CPU_JIT_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${UAE_CPU_JIT_PROCESSOR}" UAE_CPU_JIT_PROCESSOR)
set(UAE_CPU_JIT_SUPPORTED OFF)
if(UAE_CPU_JIT_PROCESSOR MATCHES "^(arm64|aarch64|x86_64|amd64)$")
    set(UAE_CPU_JIT_SUPPORTED ON)
endif()
if(UAE_CPU_JIT STREQUAL "ON" AND NOT UAE_CPU_JIT_SUPPORTED)
    message(FATAL_ERROR "UAE_CPU_JIT=ON: no JIT backend for '${UAE_CPU_JIT_PROCESSOR}' with this compiler")
endif()
if(UAE_CPU_JIT STREQUAL "ON" OR (UAE_CPU_JIT STREQUAL "AUTO" AND UAE_CPU_JIT_SUPPORTED))
    set(UAE_CPU_JIT_ENABLED ON)
else()
    set(UAE_CPU_JIT_ENABLED OFF)
endif()
message(STATUS "uae-portable-cpu: JIT ${UAE_CPU_JIT_ENABLED} (${UAE_CPU_JIT_PROCESSOR})")

set(JIT_SOURCES
    src/cpu/vm.c
    src/cpu/jit/compemu.cpp
    src/cpu/jit/compemu_support.cpp
    src/cpu/jit/compstbl.cpp
    src/cpu/jit/compemu_fpp.cpp
)
if(UAE_CPU_JIT_ENABLED)
    list(APPEND CORE_SOURCES ${JIT_SOURCES})
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set_source_files_properties(${CPUEMU_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-sign-compare -Wno-unused-variable -Wno-unused-function -Wno-parentheses -fwrapv")
    set_source_files_properties(src/cpu/newcpu.c src/cpu/newcpu_common.c src/cpu/cpummu.c src/cpu/cpummu030.c PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-unused-function -fwrapv")
endif()

add_library(uaecpu ${CORE_SOURCES})
add_library(uae-portable-cpu::uaecpu ALIAS uaecpu)

target_include_directories(uaecpu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cpu>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/softfloat>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(uaecpu PRIVATE UAE_CPU_MUSASHI_API=$<BOOL:${UAE_CPU_MUSASHI_API}>)
if(UAE_CPU_JIT_ENABLED)
    target_compile_definitions(uaecpu PRIVATE JIT USE_JIT_FPU NATMEM_OFFSET=natmem_offset)
    target_include_directories(uaecpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/jit)
    # Upstream JIT sources: warnings off. MSVC needs /bigobj for the generated
    # opcode compilers (compemu_x86.cpp alone is over 100,000 lines).
    if(MSVC)
        set(UAE_CPU_JIT_SOURCE_FLAGS "/w /bigobj")
    else()
        set(UAE_CPU_JIT_SOURCE_FLAGS "-w -fwrapv")
    endif()
    set_source_files_properties(src/cpu/jit/compemu.cpp src/cpu/jit/compemu_support.cpp
        src/cpu/jit/compstbl.cpp src/cpu/jit/compemu_fpp.cpp
        PROPERTIES COMPILE_FLAGS "${UAE_CPU_JIT_SOURCE_FLAGS}")
endif()
# Real definitions instead of C "common" symbols: the linker cannot hide a
# common symbol, which would leak internals such as regs out of uaecpu_isolated.
if(NOT MSVC)
    target_compile_options(uaecpu PRIVATE -fno-common)
endif()

if(NOT MSVC)
    target_link_libraries(uaecpu PUBLIC m)
endif()

# Symbol isolation: embedders that already define intlev(), memory_init(),
# write_log(), m68k_* (Musashi) and friends link against uaecpu_isolated, a
# relocatable object whose only global symbols are the public API.
if(UAE_CPU_ISOLATE_SYMBOLS)
    if(BUILD_SHARED_LIBS)
        message(FATAL_ERROR "UAE_CPU_ISOLATE_SYMBOLS requires BUILD_SHARED_LIBS=OFF")
    endif()
    set(UAE_CPU_EXPORT_PATTERNS "uae_cpu_*")
    if(UAE_CPU_MUSASHI_API)
        list(APPEND UAE_CPU_EXPORT_PATTERNS "m68k_*")
    endif()
    set(UAECPU_ISOLATED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/uaecpu_isolated${CMAKE_C_OUTPUT_EXTENSION})
    set(UAECPU_EXPORTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/uaecpu_exports.txt)
    set(UAECPU_EXPORTS "")
    if(UAE_CPU_ISOLATE_BY_RENAME)
        # No relocatable link: rename every internal global in a copy of the
        # archive (cmake/isolate_by_rename.cmake), so the result is an archive
        # rather than one object and is used as an imported library.
        if(NOT CMAKE_NM OR NOT CMAKE_OBJCOPY)
            message(FATAL_ERROR "UAE_CPU_ISOLATE_BY_RENAME needs nm and objcopy (CMAKE_NM, CMAKE_OBJCOPY)")
        endif()
        set(UAECPU_EXPORT_REGEX "")
        foreach(pattern ${UAE_CPU_EXPORT_PATTERNS})
            string(REPLACE "*" ".*" pattern_regex "${pattern}")
            if(UAECPU_EXPORT_REGEX)
                string(APPEND UAECPU_EXPORT_REGEX "|")
            endif()
            string(APPEND UAECPU_EXPORT_REGEX "^${pattern_regex}$")
        endforeach()
        set(UAECPU_ISOLATED_LIB
            ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}uaecpu_isolated${CMAKE_STATIC_LIBRARY_SUFFIX})
        add_custom_command(
            OUTPUT ${UAECPU_ISOLATED_LIB}
            COMMAND ${CMAKE_COMMAND}
                    -DNM=${CMAKE_NM} -DOBJCOPY=${CMAKE_OBJCOPY}
                    -DIN_LIB=$<TARGET_FILE:uaecpu> -DOUT_LIB=${UAECPU_ISOLATED_LIB}
                    -DMAP=${CMAKE_CURRENT_BINARY_DIR}/uaecpu_isolated_renames.txt
                    "-DEXPORT_REGEX=${UAECPU_EXPORT_REGEX}" -DPREFIX=uaecpu_internal_
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/isolate_by_rename.cmake
            DEPENDS uaecpu ${CMAKE_CURRENT_SOURCE_DIR}/cmake/isolate_by_rename.cmake
            COMMENT "Renaming uaecpu internals (public API only)"
            VERBATIM
        )
        add_custom_target(uaecpu_isolated_archive DEPENDS ${UAECPU_ISOLATED_LIB})
        add_library(uaecpu_isolated STATIC IMPORTED GLOBAL)
        set_target_properties(uaecpu_isolated PROPERTIES IMPORTED_LOCATION ${UAECPU_ISOLATED_LIB})
        add_dependencies(uaecpu_isolated uaecpu_isolated_archive)
        target_include_directories(uaecpu_isolated INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(uaecpu_isolated INTERFACE m)
        add_library(uae-portable-cpu::uaecpu_isolated ALIAS uaecpu_isolated)
        install(FILES ${UAECPU_ISOLATED_LIB} DESTINATION ${CMAKE_INSTALL_LIBDIR})
    elseif(APPLE)
        list(LENGTH CMAKE_OSX_ARCHITECTURES UAECPU_ARCH_COUNT)
        if(UAECPU_ARCH_COUNT GREATER 1)
            message(FATAL_ERROR "UAE_CPU_ISOLATE_SYMBOLS supports one architecture per build (CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})")
        elseif(UAECPU_ARCH_COUNT EQUAL 1)
            set(UAECPU_ISOLATED_ARCH ${CMAKE_OSX_ARCHITECTURES})
        else()
            set(UAECPU_ISOLATED_ARCH ${CMAKE_SYSTEM_PROCESSOR})
        endif()
        set(UAECPU_ISOLATED_MINVER "")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(UAECPU_ISOLATED_MINVER "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        foreach(pattern ${UAE_CPU_EXPORT_PATTERNS})
            string(APPEND UAECPU_EXPORTS "_${pattern}\n")
        endforeach()
        file(WRITE ${UAECPU_EXPORTS_FILE} "${UAECPU_EXPORTS}")
        add_custom_command(
            OUTPUT ${UAECPU_ISOLATED_OBJ}
            COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -arch ${UAECPU_ISOLATED_ARCH} ${UAECPU_ISOLATED_MINVER}
                    -Wl,-force_load,$<TARGET_FILE:uaecpu>
                    -Wl,-exported_symbols_list,${UAECPU_EXPORTS_FILE} -o ${UAECPU_ISOLATED_OBJ}
            DEPENDS uaecpu ${UAECPU_EXPORTS_FILE}
            COMMENT "Linking uaecpu_isolated (public API only)"
        )
    elseif(NOT MSVC AND CMAKE_OBJCOPY)
        foreach(pattern ${UAE_CPU_EXPORT_PATTERNS})
            string(APPEND UAECPU_EXPORTS "${pattern}\n")
        endforeach()
        file(WRITE ${UAECPU_EXPORTS_FILE} "${UAECPU_EXPORTS}")
        add_custom_command(
            OUTPUT ${UAECPU_ISOLATED_OBJ}
            COMMAND ${CMAKE_C_COMPILER} -r -nostdlib
                    -Wl,--whole-archive $<TARGET_FILE:uaecpu> -Wl,--no-whole-archive
                    -o ${UAECPU_ISOLATED_OBJ}.full
            COMMAND ${CMAKE_OBJCOPY} --wildcard --keep-global-symbols=${UAECPU_EXPORTS_FILE}
                    ${UAECPU_ISOLATED_OBJ}.full ${UAECPU_ISOLATED_OBJ}
            DEPENDS uaecpu ${UAECPU_EXPORTS_FILE}
            COMMENT "Linking uaecpu_isolated (public API only)"
        )
    else()
        message(FATAL_ERROR "UAE_CPU_ISOLATE_SYMBOLS needs Apple ld, GNU ld with objcopy, or UAE_CPU_ISOLATE_BY_RENAME")
    endif()
endif()
if(UAE_CPU_ISOLATE_SYMBOLS AND NOT UAE_CPU_ISOLATE_BY_RENAME)
    set_source_files_properties(${UAECPU_ISOLATED_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_library(uaecpu_isolated STATIC ${UAECPU_ISOLATED_OBJ})
    add_library(uae-portable-cpu::uaecpu_isolated ALIAS uaecpu_isolated)
    set_target_properties(uaecpu_isolated PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(uaecpu_isolated PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(uaecpu_isolated PUBLIC m)
    install(TARGETS uaecpu_isolated
        EXPORT uae-portable-cpu-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# Installation and exports
install(TARGETS uaecpu
    EXPORT uae-portable-cpu-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT uae-portable-cpu-targets
    FILE uae-portable-cpu-targets.cmake
    NAMESPACE uae-portable-cpu::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uae-portable-cpu
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/uae-portable-cpu-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uae-portable-cpu-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/uae-portable-cpu-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uae-portable-cpu
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/uae-portable-cpu-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/uae-portable-cpu-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uae-portable-cpu
)

if(ENABLE_TESTS)
    enable_testing()

    add_executable(test_uae_cpu tests/test_uae_cpu.c)
    target_link_libraries(test_uae_cpu PRIVATE uaecpu)
    add_test(NAME test_uae_cpu COMMAND test_uae_cpu)

    add_executable(test_host_hooks tests/test_host_hooks.c)
    target_link_libraries(test_host_hooks PRIVATE uaecpu)
    add_test(NAME host_hooks COMMAND test_host_hooks)

    # Throughput benchmark (bench/). The test only checks that every mode
    # produces the reference results; run uae_cpu_bench directly for timings.
    add_executable(uae_cpu_bench bench/uae_cpu_bench.c)
    target_link_libraries(uae_cpu_bench PRIVATE uaecpu)
    add_test(NAME bench_smoke COMMAND uae_cpu_bench --quick --repeat 1)

    if(UAE_CPU_JIT_ENABLED)
        # The same suites again with every 68020+ scenario running under the JIT.
        add_test(NAME test_uae_cpu_jit COMMAND test_uae_cpu)
        add_test(NAME host_hooks_jit COMMAND test_host_hooks)
        set_tests_properties(test_uae_cpu_jit host_hooks_jit PROPERTIES ENVIRONMENT UAE_TEST_JIT=1)
        # And once more with jit_direct_memory in a 4 GB guest reservation.
        add_test(NAME host_hooks_jit_direct COMMAND test_host_hooks)
        set_tests_properties(host_hooks_jit_direct PROPERTIES ENVIRONMENT "UAE_TEST_JIT=1;UAE_TEST_JIT_DIRECT=1")
    endif()

    if(UAE_CPU_MUSASHI_API)
        add_executable(test_basic tests/test_basic.c)
        target_link_libraries(test_basic PRIVATE uaecpu)

        add_executable(test_musashi_suite tests/test_musashi_suite.c)
        target_link_libraries(test_musashi_suite PRIVATE uaecpu)

        add_executable(test_m68k_rs_suite tests/test_m68k_rs_suite.c)
        target_link_libraries(test_m68k_rs_suite PRIVATE uaecpu)

        add_test(NAME test_basic COMMAND test_basic)
        add_test(NAME musashi_68000 COMMAND test_musashi_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/musashi/mc68000 68000)
        add_test(NAME musashi_68040 COMMAND test_musashi_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/musashi/mc68040 68040)
        add_test(NAME m68k_rs_coverage COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra/coverage/bin 68040)
        add_test(NAME m68k_rs_extra COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra 68040)

        if(UAE_CPU_JIT_ENABLED)
            # 68040 fixture suites with the JIT enabled. The m68k-rs driver maps its
            # RAM/ROM into a flat JIT window, so those programs are translated; the
            # Musashi fixtures use callback-only memory and exercise the dispatcher's
            # interpreter fallback.
            add_test(NAME musashi_68040_jit COMMAND test_musashi_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/musashi/mc68040 68040)
            add_test(NAME m68k_rs_coverage_jit COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra/coverage/bin 68040)
            add_test(NAME m68k_rs_extra_jit COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra 68040)
            set_tests_properties(musashi_68040_jit m68k_rs_coverage_jit m68k_rs_extra_jit PROPERTIES ENVIRONMENT UAE_TEST_JIT=1)
            # m68k-rs again with jit_direct_memory: RAM and ROM are accessed inline.
            add_test(NAME m68k_rs_coverage_jit_direct COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra/coverage/bin 68040)
            add_test(NAME m68k_rs_extra_jit_direct COMMAND test_m68k_rs_suite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/m68k-rs/extra 68040)
            set_tests_properties(m68k_rs_coverage_jit_direct m68k_rs_extra_jit_direct PROPERTIES ENVIRONMENT "UAE_TEST_JIT=1;UAE_TEST_JIT_DIRECT=1")
        endif()
    endif()

    if(UAE_CPU_ISOLATE_SYMBOLS)
        # Links the hook tests plus deliberately clashing host symbols against
        # the isolated library: a leaked internal symbol fails the link.
        add_executable(test_isolated_link tests/test_host_hooks.c tests/isolation_collisions.c)
        target_link_libraries(test_isolated_link PRIVATE uaecpu_isolated)
        if(UAE_CPU_MUSASHI_API)
            target_compile_definitions(test_isolated_link PRIVATE UAE_TEST_MUSASHI_API=1)
        endif()
        add_test(NAME isolated_link COMMAND test_isolated_link)
    endif()
endif()
