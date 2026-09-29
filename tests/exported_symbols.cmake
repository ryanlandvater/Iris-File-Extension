# Decision 4.0-D, as a test.
#
# The exported surface is the semantic API and nothing below it. The generated
# block layer (Iris::File::blocks, ::vtables, ::constants) must export zero
# symbols from a shared build: it is pure field arithmetic, a consumer reaches
# it through IFE_HEADER_ONLY, and every exported symbol is a thing that cannot
# change without breaking someone -- which a *generated* layer must be free to
# do whenever the schema does.
#
# Hidden-by-default visibility makes this true today by accident of the CMake
# setting. This test makes it true on purpose, so a stray IFE_EXPORT on a
# generated definition, or a change to CXX_VISIBILITY_PRESET, fails a build
# rather than quietly widening the ABI.

if(NOT LIBRARY)
    message(FATAL_ERROR "exported_symbols.cmake: LIBRARY is required")
endif()
if(NOT EXISTS "${LIBRARY}")
    message(FATAL_ERROR "no library at ${LIBRARY}")
endif()

find_program(NM_TOOL NAMES nm llvm-nm)
if(NOT NM_TOOL)
    message(STATUS "nm not found; skipping the exported-symbol check")
    return()
endif()

# -g: external (exported) symbols only. -C: demangle, so the loop below can
# name the namespace it guards (Iris::File::blocks) rather than an Itanium
# spelling of it — an ABI detail that would have to be rewritten for a
# compiler that mangles differently.
execute_process(
    COMMAND "${NM_TOOL}" -g -C "${LIBRARY}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE nm_error RESULT_VARIABLE nm_code
)
if(NOT nm_code EQUAL 0)
    message(FATAL_ERROR "nm failed on ${LIBRARY}:\n${nm_error}")
endif()

# `nm -g -C` prints "<address> <type> <name>"; type U is undefined — an
# import, not an export — and only a definition widens the ABI.
string(REPLACE "\n" ";" lines "${symbols}")
set(leaked "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +(.*)$" OR CMAKE_MATCH_1 STREQUAL "U")
        continue()
    endif()
    # The PUBLIC API is namespace Iris::File (e.g. Iris::File::Parser), so the
    # bare namespace does not mean "the generated layer". The generated layer
    # is the three sub-namespaces Iris::File::blocks / ::vtables / ::constants,
    # and a symbol DEFINED in one carries that namespace at the FRONT of its
    # name — so anchor there. Matching the namespace anywhere would also hit it
    # as a template ARGUMENT: std::set<std::pair<Offset,
    # Iris::File::constants::RecoveryCodes>> demangles to exactly that inside a
    # std::_Rb_tree<...> symbol, which is not a generated-layer export (and
    # which libstdc++ leaves externally visible where libc++ does not).
    if(CMAKE_MATCH_2 MATCHES "^Iris::File::(blocks|vtables|constants)(::|$)")
        list(APPEND leaked "${line}")
    endif()
endforeach()

list(LENGTH leaked count)
if(count GREATER 0)
    string(REPLACE ";" "\n  " detail "${leaked}")
    message(FATAL_ERROR
        "${count} generated-layer symbol(s) are exported from ${LIBRARY}.\n"
        "Decision 4.0-D keeps Iris::File::blocks/vtables/constants out of the ABI; "
        "consumers reach them with IFE_HEADER_ONLY.\n  ${detail}")
endif()

message(STATUS "exported symbols: the generated layer stays out of the ABI")
