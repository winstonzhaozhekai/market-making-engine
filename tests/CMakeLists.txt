include(GoogleTest)

# Helper: declare a test executable, link to GTest + the relevant target(s),
# and register all TEST() blocks with ctest via gtest_discover_tests.
function(mme_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            ${ARGN}
            GTest::gtest_main
            mme_warnings
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    )
    gtest_discover_tests(${name})
endfunction()

mme_add_test(test_tick_conversion)
mme_add_test(test_accounting)
mme_add_test(test_matching_engine     mme_core)
mme_add_test(test_matching_engine_scaling mme_core)
mme_add_test(test_risk_manager        mme_core)
mme_add_test(test_strategy_behavior   mme_core)
mme_add_test(test_mm_quote_amend      mme_core)
mme_add_test(test_hot_path_allocations mme_core)
mme_add_test(test_spsc_ring)
mme_add_test(test_spsc_logger)
mme_add_test(test_latency_scheduler)
mme_add_test(test_queue_reactive_lob)
mme_add_test(test_itch_parser)
mme_add_test(test_queue_reactive_simulator mme_core)
mme_add_test(test_queue_reactive_realism   mme_core)
mme_add_test(test_binary_log_roundtrip mme_core)
mme_add_test(test_determinism         mme_core)
mme_add_test(test_latency_monotonicity mme_core)
mme_add_test(test_ws_protocol         mme_ws)

# M7 done-when: release-build templated MarketMakerT<S,L> hot path has no
# indirect-call (vtable dispatch) instructions inside the noinline
# tick_once<...> wrappers in bench_engine. Release-only; the script is a
# no-op pass on non-Linux/macOS or non-x86_64/arm64 hosts.
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_test(
        NAME M7_no_virtual_dispatch
        COMMAND ${CMAKE_SOURCE_DIR}/tests/check_no_virtual_dispatch.sh
                $<TARGET_FILE:bench_engine>
    )
endif()
