# One configured identity feeds the bundles, compiled provenance and packages.
string(TOUPPER "${PROJECT_NAME}" distribution_prefix)
set(distribution_default_build "1")
if(DEFINED ENV{GITHUB_RUN_ID} AND NOT "$ENV{GITHUB_RUN_ID}" STREQUAL "")
    set(distribution_default_build "$ENV{GITHUB_RUN_ID}.$ENV{GITHUB_RUN_ATTEMPT}")
endif()
set(${distribution_prefix}_BUILD_NUMBER "${distribution_default_build}" CACHE STRING
    "Build identifier: positive integer, optionally followed by .attempt")
if(NOT "${${distribution_prefix}_BUILD_NUMBER}" MATCHES "^[1-9][0-9]*(\\.[1-9][0-9]*)?$")
    message(FATAL_ERROR "${distribution_prefix}_BUILD_NUMBER must be a positive integer or run.attempt")
endif()
set(${distribution_prefix}_DISTRIBUTION_VERSION
    "${PROJECT_VERSION}-build.${${distribution_prefix}_BUILD_NUMBER}"
    CACHE INTERNAL "Version used in distribution filenames" FORCE)
set(${distribution_prefix}_DISPLAY_VERSION
    "${PROJECT_VERSION} (build ${${distribution_prefix}_BUILD_NUMBER})")
find_package(Git QUIET)
set(distribution_source_revision "unknown")
set(distribution_source_state "unavailable")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE distribution_source_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE distribution_git_result)
    if(distribution_git_result EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal -- .
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE distribution_changes)
        set(distribution_source_state "clean")
        if(NOT distribution_changes STREQUAL "")
            set(distribution_source_state "dirty")
        endif()
    else()
        set(distribution_source_revision "unknown")
    endif()
endif()
set(distribution_identity "{\n  \"product\": \"${PROJECT_NAME}\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"build_number\": \"${${distribution_prefix}_BUILD_NUMBER}\",\n  \"distribution_version\": \"${${distribution_prefix}_DISTRIBUTION_VERSION}\",\n  \"source_revision\": \"${distribution_source_revision}\",\n  \"source_state\": \"${distribution_source_state}\"\n}\n")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-build-identity.json"
    CONTENT "${distribution_identity}")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/${PROJECT_NAME}BuildIdentity.cpp"
    CONTENT "// Generated at configure time; changing identity rebuilds each wrapper.\nextern const char ${PROJECT_NAME}BuildIdentity[];\nconst char ${PROJECT_NAME}BuildIdentity[] = R\"identity(${distribution_identity})identity\";\n")
function(instrument_distribution_identity target)
    target_sources(${target} PRIVATE "${PROJECT_BINARY_DIR}/${PROJECT_NAME}BuildIdentity.cpp")
    foreach(format IN LISTS ARGN)
        add_custom_command(TARGET ${target}_${format} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PROJECT_BINARY_DIR}/${PROJECT_NAME}-build-identity.json"
                "${PROJECT_BINARY_DIR}/${PROJECT_NAME}_artefacts/$<CONFIG>/${format}/${PROJECT_NAME}-build-identity.json"
            VERBATIM)
    endforeach()
endfunction()
