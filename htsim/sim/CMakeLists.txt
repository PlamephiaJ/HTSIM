cmake_minimum_required(VERSION 3.16)
include(FetchContent)

project(htsim_project LANGUAGES CXX)
option(ENABLE_TESTS "enable the test suite" OFF)
option(ENABLE_NETWORK_DECISION_TESTS "enable standalone network decision tests" OFF)

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensures GNU extensions are disabled
set(CMAKE_CXX_FLAGS_RELEASE -O3) # Prevent -DNDEBUG, htsim requires 
                                 # asserts to be enabled.
# Set compiler flags
add_compile_options(
    -Wall
    -g
    -Wsign-compare
    -Wuninitialized
    -fPIE
    -Wno-deprecated
    -O3
)
# Optionally enable sanitizers
# -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined"

if ( ENABLE_TESTS ) 
    # Fetch GoogleTest
    FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
    enable_testing()
endif()

# Function to add a symlink creation step for an executable. 
# Used to create a symlink in the source directory to the executable 
# in the build directory for easier access and backward compatibility.
function(add_symlink_target EXECUTABLE TARGET_DIR)
    add_custom_command(
        TARGET ${EXECUTABLE}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove ${TARGET_DIR}/${EXECUTABLE} # Remove existing symlink
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE:${EXECUTABLE}>  # Path to the executable
            ${TARGET_DIR}/${EXECUTABLE}  # Path for the symlink
        COMMENT "Removing and recreating symlink for ${EXECUTABLE} in ${TARGET_DIR}"
    )
endfunction()

# Define source files
set(SOURCE_FILES
    aeolusqueue.cpp
    callback_pipe.cpp
    cbr.cpp
    cbrpacket.cpp
    clock.cpp
    cnppacket.cpp
    compositeprioqueue.cpp
    compositequeue.cpp
    config.cpp
    cpqueue.cpp
    dcqcn.cpp
    dcqcn_logger.cpp
    dctcp.cpp
    dctcp_transfer.cpp
    ecnprioqueue.cpp
    ecnqueue.cpp
    eqds.cpp
    eqds_logger.cpp
    eqdspacket.cpp
    eth_pause_packet.cpp
    eventlist.cpp
    exoqueue.cpp
    fairpullqueue.cpp
    hpcc.cpp
    hpccpacket.cpp
    logfile.cpp
    loggers.cpp
    metric.cpp
    data_collector.cpp
    meter.cpp
    mtcp.cpp
    ndp.cpp
    ndp_transfer.cpp
    ndppacket.cpp
    ndptunnel.cpp
    ndptunnelpacket.cpp
    network.cpp
    oversubscribed_cc.cpp
    pciemodel.cpp
    pipe.cpp
    priopullqueue.cpp
    prioqueue.cpp
    qcn.cpp
    queue.cpp
    queue_lossless.cpp
    queue_lossless_input.cpp
    queue_lossless_output.cpp
    randomqueue.cpp
    rng.cpp
    roce.cpp
    rocepacket.cpp
    buffer_reps.cpp
    route.cpp
    routetable.cpp
    sent_packets.cpp
    strack.cpp
    strackpacket.cpp
    swift.cpp
    swift_scheduler.cpp
    swiftpacket.cpp
    switch.cpp
    tcp.cpp
    tcp_periodic.cpp
    tcp_transfer.cpp
    tcppacket.cpp
    trigger.cpp
    uec.cpp
    uec_logger.cpp
    uecpacket.cpp
    uec_mp.cpp
    uec_pdcses.cpp
    logsim-interface.cpp
    compute_event.cpp
    null_event.cpp
    atlahs_htsim_api.cpp
    atlahs_dragonfly_api.cpp
    rank_placement.cpp
    messagelet_config.cpp
    network_decision.cpp
    group_routing_agent.cpp
    gra_static_graph.cpp
    gra_goal_graph.cpp
    flow_routing_config.cpp
    messagelet_runtime.cpp
)

# Create static library
add_library(htsim STATIC ${SOURCE_FILES})
target_include_directories(htsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) # Needed for header-only libraries (e.g., loggertypes.h).

# Add subdirectories
add_subdirectory(datacenter)

# Define executables and link with htsim library
add_executable(parse_output parse_output.cpp)
target_link_libraries(parse_output PRIVATE htsim)

if (ENABLE_TESTS OR ENABLE_NETWORK_DECISION_TESTS)
    enable_testing()
    add_executable(gra_static_graph_test gra_static_graph_test.cpp)
    target_link_libraries(gra_static_graph_test PRIVATE htsim_dc htsim htsim_dc htsim)
    add_test(NAME gra_static_graph_test COMMAND gra_static_graph_test)
    set_tests_properties(gra_static_graph_test PROPERTIES TIMEOUT 30)
    add_executable(network_decision_test network_decision_test.cpp)
    target_link_libraries(network_decision_test PRIVATE htsim_dc htsim htsim_dc htsim)
    add_test(NAME network_decision_test COMMAND network_decision_test)
    set_tests_properties(network_decision_test PROPERTIES TIMEOUT 30)
    foreach(GRANULARITY flow packet)
        add_test(NAME network_decision_${GRANULARITY}_test COMMAND network_decision_test ${GRANULARITY})
        set_tests_properties(network_decision_${GRANULARITY}_test PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME network_decision_transparent_test COMMAND network_decision_test transparent)
    set_tests_properties(network_decision_transparent_test PROPERTIES TIMEOUT 30)
    foreach(ENTRY_TEST entry entry_admission)
        add_test(NAME network_decision_${ENTRY_TEST}_test COMMAND network_decision_test ${ENTRY_TEST})
        set_tests_properties(network_decision_${ENTRY_TEST}_test PROPERTIES TIMEOUT 30)
    endforeach()
endif()

if ( ENABLE_TESTS )
    # Unit tests.
    set(UNIT_TEST_FILES
        pipe_test
    )

    foreach(UNIT_TEST_FILE ${UNIT_TEST_FILES}) 
        add_executable(${UNIT_TEST_FILE} ${UNIT_TEST_FILE}.cpp)
        target_link_libraries(${UNIT_TEST_FILE} PRIVATE htsim gtest_main gmock_main)
        gtest_discover_tests(${UNIT_TEST_FILE}
            PROPERTIES LABELS "unit"
            PROPERTIES TIMEOUT 60
        )
    endforeach()
endif()

# Always print ENABLE_TESTS status, so folks don't miss it.
add_custom_target(
  always_run ALL
  COMMAND ${CMAKE_COMMAND} -E echo "ENABLE_TESTS=${ENABLE_TESTS}"
)
