add_scylla_test(cross_shard_barrier_test
  KIND UNIT)
add_scylla_test(lsa_sync_eviction_test
  KIND UNIT)
add_scylla_test(lsa_async_eviction_test
  KIND UNIT)
add_scylla_test(row_cache_alloc_stress_test
  KIND UNIT)
add_scylla_test(row_cache_stress_test
  KIND UNIT)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/suite.yaml")
  set(scylla_tests "${scylla_tests}" PARENT_SCOPE)
endif()
