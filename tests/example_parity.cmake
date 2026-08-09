# 4.4's "Done when", as a test.
#
# The same example source, compiled once against the hand-written layer and
# once against the generated runtime -- differing only in which header it
# includes -- run on one slide file written by the shipped encoder. The port is
# accepted only if the two produce byte-identical output.
#
# Driven from CMake rather than a C++ test because the three binaries cannot be
# linked together: v1 and IFE_Runtime define the same four entry points.

if(NOT WRITER OR NOT EXAMPLE_V1 OR NOT EXAMPLE_RUNTIME OR NOT WORK)
    message(FATAL_ERROR "example_parity.cmake: WRITER, EXAMPLE_V1, EXAMPLE_RUNTIME and WORK are required")
endif()

set(SLIDE "${WORK}/example_parity.iris")

execute_process(COMMAND "${WRITER}" "${SLIDE}" RESULT_VARIABLE wrote)
if(NOT wrote EQUAL 0)
    message(FATAL_ERROR "the v1 slide writer failed (${wrote})")
endif()

execute_process(
    COMMAND "${EXAMPLE_V1}" "${SLIDE}"
    OUTPUT_VARIABLE out_v1 ERROR_VARIABLE err_v1 RESULT_VARIABLE code_v1
)
execute_process(
    COMMAND "${EXAMPLE_RUNTIME}" "${SLIDE}"
    OUTPUT_VARIABLE out_runtime ERROR_VARIABLE err_runtime RESULT_VARIABLE code_runtime
)

if(NOT code_v1 EQUAL 0)
    message(FATAL_ERROR "the v1 example failed (${code_v1}):\n${out_v1}${err_v1}")
endif()
if(NOT code_runtime EQUAL 0)
    message(FATAL_ERROR "the runtime example failed (${code_runtime}):\n${out_runtime}${err_runtime}")
endif()

if(NOT out_v1 STREQUAL out_runtime)
    message(FATAL_ERROR
        "the two layers disagree about the same slide.\n"
        "---- hand-written ----\n${out_v1}"
        "---- generated ----\n${out_runtime}")
endif()

message(STATUS "example parity: both layers produced identical output")
