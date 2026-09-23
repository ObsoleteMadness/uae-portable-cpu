# Symbol isolation for toolchains without relocatable (-r) linking.
#
# LLVM's COFF linker (lld-link, used by the llvm-mingw / CLANGARM64
# toolchains) cannot produce a relocatable object, so the partial link that
# the Apple and GNU paths use to hide the core's internals is unavailable.
# Instead, every global symbol the archive defines, apart from the public API,
# is renamed with a prefix. objcopy --redefine-syms renames definitions and
# references alike, so the members still link against each other, while an
# embedder's own intlev(), write_log() or softfloat routines no longer clash.
# Symbols the archive only references (the C and C++ runtimes) are untouched.
#
# Run with cmake -P and these variables:
#   NM, OBJCOPY: Tools matching the target's object format.
#   IN_LIB: The core's static library.
#   OUT_LIB: The isolated static library to write.
#   MAP: Where to write the rename map (kept for inspection).
#   EXPORT_REGEX: Symbols matching this stay global under their own name.
#   PREFIX: Prefix given to every other defined global symbol.

foreach(var NM OBJCOPY IN_LIB OUT_LIB MAP EXPORT_REGEX PREFIX)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "isolate_by_rename.cmake: ${var} is not set")
    endif()
endforeach()

# Global symbols defined anywhere in the archive
execute_process(
    COMMAND ${NM} -g --defined-only ${IN_LIB}
    OUTPUT_VARIABLE nm_out
    RESULT_VARIABLE nm_rc
)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "isolate_by_rename.cmake: ${NM} failed on ${IN_LIB}")
endif()

# nm prints "<value> <type> <name>" per symbol and "<member>:" per member
string(REPLACE "\n" ";" nm_lines "${nm_out}")
list(FILTER nm_lines INCLUDE REGEX "^[0-9A-Fa-f]* *[A-Za-z] [^ ]+$")
list(TRANSFORM nm_lines REPLACE "^[0-9A-Fa-f]* *[A-Za-z] " "")
# The public API keeps its name; a symbol defined in several members (inline
# and template instantiations) is mapped once
list(FILTER nm_lines EXCLUDE REGEX "${EXPORT_REGEX}")
list(REMOVE_DUPLICATES nm_lines)
set(map "")
foreach(sym IN LISTS nm_lines)
    string(APPEND map "${sym} ${PREFIX}${sym}\n")
endforeach()

file(WRITE ${MAP} "${map}")
execute_process(
    COMMAND ${OBJCOPY} --redefine-syms=${MAP} ${IN_LIB} ${OUT_LIB}
    RESULT_VARIABLE objcopy_rc
)
if(NOT objcopy_rc EQUAL 0)
    message(FATAL_ERROR "isolate_by_rename.cmake: ${OBJCOPY} failed on ${IN_LIB}")
endif()
