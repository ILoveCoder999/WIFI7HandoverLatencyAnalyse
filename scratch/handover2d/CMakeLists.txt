include_directories(${CMAKE_CURRENT_SOURCE_DIR})

build_exec (
    EXECNAME latency-test-v2
    SOURCE_FILES interferer-application.cc interferer-application-helper.cc my-udp-client.cc my-udp-client-helper.cc utils.cc sta-logger.cc latency-test-v2.cc 
    LIBRARIES_TO_LINK ${ns3-libs}
    EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/latency-test-v2
)

build_exec (
    EXECNAME handover
    SOURCE_FILES interferer-application.cc interferer-application-helper.cc my-udp-client.cc my-udp-client-helper.cc utils.cc str-utils.cc sta-logger.cc assoc-logger.cc roaming-manager.cc wifi-controller.cc handover.cc
    LIBRARIES_TO_LINK ${ns3-libs}
    EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/handover
)

build_exec (
    EXECNAME reassociation_test
    SOURCE_FILES interferer-application.cc interferer-application-helper.cc my-udp-client.cc my-udp-client-helper.cc utils.cc str-utils.cc sta-logger.cc assoc-logger.cc reassociation_test.cc
    LIBRARIES_TO_LINK ${ns3-libs}
    EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/reassociation_test
)