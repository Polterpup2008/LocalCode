# GUI targets can finish linking concurrently into the same bin directory.
# windeployqt's copy/update operations are not atomic across those processes.
if(NOT EXISTS "${LOCALCODE_DEPLOY_TOOL}" OR NOT EXISTS "${LOCALCODE_DEPLOY_EXECUTABLE}")
    message(FATAL_ERROR "Select an existing Qt deployment tool and target executable")
endif()
get_filename_component(deploy_directory "${LOCALCODE_DEPLOY_EXECUTABLE}" DIRECTORY)
file(LOCK "${deploy_directory}/.localcode-qt-deploy.lock"
    GUARD PROCESS TIMEOUT 120 RESULT_VARIABLE lock_result)
if(NOT "${lock_result}" STREQUAL "0")
    message(FATAL_ERROR "Cannot lock Qt runtime deployment: ${lock_result}")
endif()
execute_process(COMMAND "${LOCALCODE_DEPLOY_TOOL}"
    --no-translations --no-system-d3d-compiler --no-opengl-sw
    "${LOCALCODE_DEPLOY_EXECUTABLE}"
    TIMEOUT 90 RESULT_VARIABLE deploy_result)
if(NOT "${deploy_result}" STREQUAL "0")
    message(FATAL_ERROR "Qt runtime deployment failed: ${deploy_result}")
endif()
