set(LINK_MEM_PER_JOB_DEFAULT 4 CACHE INTERNAL "Maximum memory used by each link job for non-release build (in GiB)")
set(LINK_MEM_PER_JOB_RELEASE 18 CACHE INTERNAL "Maximum memory used by each link job for release build (in GiB)")

cmake_host_system_information(
  RESULT _total_mem_mb
  QUERY AVAILABLE_PHYSICAL_MEMORY)
math(EXPR _total_mem_gb "${_total_mem_mb} / 1024")
math(EXPR _link_pool_depth_default "${_total_mem_gb} / ${LINK_MEM_PER_JOB_DEFAULT}")
math(EXPR _link_pool_depth_release "${_total_mem_gb} / ${LINK_MEM_PER_JOB_RELEASE}")
if(_link_pool_depth_default EQUAL 0)
  set(_link_pool_depth_default 1)
endif()
if(_link_pool_depth_release EQUAL 0)
  set(_link_pool_depth_release 1)
endif()
set_property(
  GLOBAL
  APPEND
  PROPERTY JOB_POOLS
    link_pool_default=${_link_pool_depth_default}
    link_pool_release=${_link_pool_depth_release}
    submodule_pool=1)
# it is not allowed to set JOB_POOL_LINK using generator expression, so we use
# use the default link pool globally to perform link jobs if RelWithDebInfo is
# not selected, otherwise set JOB_POOL_LINK on a per-target basis.
if(is_multi_config)
  if(NOT RelWithDebInfo IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_JOB_POOL_LINK link_pool_default)
  endif()
else()
  if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(CMAKE_JOB_POOL_LINK link_pool_default)
  endif()
endif()
