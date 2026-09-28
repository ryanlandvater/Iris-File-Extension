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

# -g: external (exported) symbols only. Mangled `Iris::File::blocks` is
# `4Iris4File6blocks` (Itanium length-prefixed); see the match in the loop below.
execute_process(
    COMMAND "${NM_TOOL}" -g "${LIBRARY}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE nm_error RESULT_VARIABLE nm_code
)
if(NOT nm_code EQUAL 0)
    message(FATAL_ERROR "nm failed on ${LIBRARY}:\n${nm_error}")
endif()

# Undefined symbols are imports, not exports; only defined ones widen the ABI.
string(REPLACE "\n" ";" lines "${symbols}")
set(leaked "")
foreach(line IN LISTS lines)
    # The PUBLIC API is namespace Iris::File (e.g. Iris::File::Recovery), so the
    # bare namespace does not mean "the generated layer" — matching it would
    # flag every exported symbol. The generated block layer lives in the THREE
    # sub-namespaces Iris::File::blocks / ::vtables / ::constants, whose mangled
    # spellings are the Itanium length-prefixed "4Iris4File6blocks" /
    # "4Iris4File7vtables" / "4Iris4File9constants". Match those.
    if(line MATCHES "4Iris4File(6blocks|7vtables|9constants)" AND NOT line MATCHES " U ")
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
