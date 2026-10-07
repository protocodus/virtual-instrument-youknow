if(NOT DEFINED NORMAL_AUDIT OR NOT DEFINED WORK_AUDIT)
    message(FATAL_ERROR "NORMAL_AUDIT and WORK_AUDIT executables are required")
endif()

# Exercise CLI routing without starting a wall-clock benchmark. In particular,
# Original timing must be discoverable on both builds, reject malformed input,
# and refuse the instrumented executable before entering its timing path.
foreach(audit IN ITEMS "${NORMAL_AUDIT}" "${WORK_AUDIT}")
    execute_process(
        COMMAND "${audit}" --help
        RESULT_VARIABLE help_result
        OUTPUT_VARIABLE help_output
        ERROR_VARIABLE help_error
        TIMEOUT 10)
    if(NOT "${help_result}" STREQUAL "0"
       OR NOT help_output MATCHES "--original-cpu-benchmark"
       OR NOT help_output MATCHES "--cpu-benchmark uses Direct"
       OR NOT help_output MATCHES "--original-cpu-benchmark uses Original"
       OR NOT help_output MATCHES "CPU defaults: 48000 Hz, requested factor 1")
        message(FATAL_ERROR
            "oversampling CPU help contract failed (${audit}, ${help_result}):\n"
            "${help_output}${help_error}")
    endif()
endforeach()

function(expect_cpu_cli_error executable expected_result expected_message)
    execute_process(
        COMMAND "${executable}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10)
    if(NOT "${result}" STREQUAL "${expected_result}"
       OR NOT "${output}${error}" MATCHES "${expected_message}")
        message(FATAL_ERROR
            "oversampling CPU argument contract failed (${ARGN}, ${result}):\n"
            "${output}${error}")
    endif()
    if(output MATCHES "protocol host_rate=" OR output MATCHES "cpu_timing ")
        message(FATAL_ERROR "invalid CPU arguments started a benchmark: ${ARGN}")
    endif()
endfunction()

foreach(mode IN ITEMS --cpu-benchmark --original-cpu-benchmark)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "expected an integer CPU benchmark argument"
        ${mode} 48000x)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "expected an integer CPU benchmark argument"
        ${mode} 99999999999999999999)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "expected an integer CPU benchmark argument"
        ${mode} 48000 invalid)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "CPU benchmark rate or factor is unsupported"
        ${mode} 7999)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "CPU benchmark rate or factor is unsupported"
        ${mode} 768001)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 1 "CPU benchmark rate or factor is unsupported"
        ${mode} 48000 3)
    expect_cpu_cli_error("${NORMAL_AUDIT}" 2 "usage:"
        ${mode} 48000 1 extra)
    expect_cpu_cli_error("${WORK_AUDIT}" 2 "requires YouKnowOversamplingAudit"
        ${mode} 48000 1)
endforeach()

execute_process(
    COMMAND "${NORMAL_AUDIT}" --fingerprint
    RESULT_VARIABLE normal_result
    OUTPUT_VARIABLE normal_fingerprint
    ERROR_VARIABLE normal_error)
if(NOT normal_result EQUAL 0)
    message(FATAL_ERROR
        "normal oversampling fingerprint failed (${normal_result}): ${normal_error}")
endif()

execute_process(
    COMMAND "${WORK_AUDIT}" --fingerprint
    RESULT_VARIABLE work_result
    OUTPUT_VARIABLE work_fingerprint
    ERROR_VARIABLE work_error)
if(NOT work_result EQUAL 0)
    message(FATAL_ERROR
        "instrumented oversampling fingerprint failed (${work_result}): ${work_error}")
endif()

if(NOT normal_fingerprint STREQUAL work_fingerprint)
    message(FATAL_ERROR
        "work instrumentation changed the raw-float fingerprint\n"
        "normal:\n${normal_fingerprint}"
        "instrumented:\n${work_fingerprint}")
endif()

execute_process(
    COMMAND "${WORK_AUDIT}" --self-test
    RESULT_VARIABLE counter_result
    OUTPUT_VARIABLE counter_output
    ERROR_VARIABLE counter_error)
if(NOT counter_result EQUAL 0)
    message(FATAL_ERROR
        "oversampling work-counter contract failed (${counter_result}):\n"
        "${counter_output}${counter_error}")
endif()

message(STATUS "oversampling CPU CLI, fingerprint parity and work-counter algebra passed")
