####
# target/test-target.cmake:
#
# This target sets up a test target for testing purposes.
####


function(add_global_target TARGET)
    add_custom_target("${TARGET}" COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/global-test")
endfunction(add_global_target)

function(add_deployment_target MODULE TARGET SOURCES DIRECT_DEPENDENCIES FULL_DEPENDENCY_LIST)
    add_custom_target("${MODULE}_${TARGET}" COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/deployment-test")
endfunction()

function(add_module_target MODULE TARGET SOURCES DEPENDENCIES)
    run_ac_set("${SOURCES}" autocoder/test)
    add_custom_target("${MODULE}_${TARGET}" COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/${MODULE}-test"
                      DEPENDS ${AC_GENERATED})
endfunction(add_module_target)
