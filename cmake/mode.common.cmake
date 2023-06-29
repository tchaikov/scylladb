set(disabled_warnings
  c++11-narrowing
  mismatched-tags
  overloaded-virtual
  unsupported-friend)
include(CheckCXXCompilerFlag)
foreach(warning ${disabled_warnings})
  check_cxx_compiler_flag("-Wno-${warning}" _warning_supported_${warning})
  if(_warning_supported_${warning})
    list(APPEND _supported_warnings ${warning})
  endif()
endforeach()
list(TRANSFORM _supported_warnings PREPEND "-Wno-")
string(JOIN " " CMAKE_CXX_FLAGS
  "-Wall"
  "-Werror"
  "-Wno-error=deprecated-declarations"
  "-Wimplicit-fallthrough"
  ${_supported_warnings})

function(default_target_arch arch)
  set(x86_instruction_sets i386 i686 x86_64)
  if(CMAKE_SYSTEM_PROCESSOR IN_LIST x86_instruction_sets)
    set(${arch} "westmere" PARENT_SCOPE)
  elseif(CMAKE_SYSTEM_PROCESSOR EQUAL "aarch64")
    set(${arch} "armv8-a+crc+crypto" PARENT_SCOPE)
  else()
    set(${arch} "" PARENT_SCOPE)
  endif()
endfunction()

default_target_arch(target_arch)
if(target_arch)
    string(APPEND CMAKE_CXX_FLAGS " -march=${target_arch}")
endif()

math(EXPR _stack_usage_threshold_in_bytes "${stack_usage_threshold_in_KB} * 1024")
set(_stack_usage_threshold_flag "-Wstack-usage=${_stack_usage_threshold_in_bytes}")
check_cxx_compiler_flag(${_stack_usage_threshold_flag} _stack_usage_flag_supported)
if(_stack_usage_flag_supported)
  string(APPEND CMAKE_CXX_FLAGS " ${_stack_usage_threshold_flag}")
endif()

set(pgo_opts "")
# only IR and CSIR are supported. because the IR (middle end) based instrumentation
# is superier than the frontend based instrumentation when profiling executable for
# optimization purposes.
set(Scylla_BUILD_INSTRUMENTED OFF CACHE STRING
    "Build ScyllaDB with PGO instrumentation. May be specified as IR, CSIR")
if(Scylla_BUILD_INSTRUMENTED)
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/${Scylla_BUILD_INSTRUMENTED}" Scylla_PROFILE_DATA_DIR)
  if(Scylla_BUILD_INSTRUMENTED STREQUAL "IR")
    # instrument code at IR level, also known as the regular PGO
    string(APPEND pgo_opts " -fprofile-generate=\"${Scylla_PROFILE_DATA_DIR}\"")
  elseif(Scylla_BUILD_INSTRUMENTED STREQUAL "CSIR")
    # instrument code with Context Sensitive IR, also known as CSPGO.
    string(APPEND pgo_opts " -fcs-profile-generate=\"${Scylla_PROFILE_DATA_DIR}\"")
  else()
    message(FATAL_ERROR "Unknown Scylla_BUILD_INSTRUMENTED: ${}")
  endif()
  string(APPEND CMAKE_CXX_FLAGS "${pgo_opts}")
  string(APPEND CMAKE_EXE_LINKER_FLAGS "${pgo_opts}")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS "${pgo_opts}")
endif()
