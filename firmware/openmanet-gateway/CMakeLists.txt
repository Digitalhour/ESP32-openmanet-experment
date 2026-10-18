cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiao_halow_gateway)

# Reapplies a local workaround to the vendored morsemicro__halow component
# (see patch_vendored_halow.py's own docstring for the full why) after the
# component manager has fetched it - project() above is what triggers that
# fetch, so this must run after it, not before. managed_components/ is
# gitignored and re-fetched clean on every checkout/dependency bump, so
# without this the fix silently disappears; the script is idempotent (checks
# for its own marker comment first) and fails loudly rather than guessing if
# the vendored file's shape has changed underneath it.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env python3 "${CMAKE_SOURCE_DIR}/patch_vendored_halow.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE patch_vendored_halow_result
    OUTPUT_VARIABLE patch_vendored_halow_output
    ERROR_VARIABLE patch_vendored_halow_error)
message(STATUS "${patch_vendored_halow_output}")
if(NOT patch_vendored_halow_result EQUAL 0)
    message(FATAL_ERROR "patch_vendored_halow.py failed: ${patch_vendored_halow_error}")
endif()
