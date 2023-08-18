set(disabled_warnings
  c++11-narrowing
  mismatched-tags
  overloaded-virtual
  unsupported-friend
  unused-parameter
  missing-field-initializers
  deprecated-copy
  ignored-qualifiers)
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
  "-Wextra"
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
endif()

if(pgo_opts)
  string(APPEND CMAKE_CXX_FLAGS "${pgo_opts}")
  string(APPEND CMAKE_EXE_LINKER_FLAGS "${pgo_opts}")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS "${pgo_opts}")
endif()

if(Scylla_BUILD_INSTRUMENTED)
  # options dependent on Scylla_BUILD_INSTRUMENTED
  set(Scylla_PROFDATA_FILE "" CACHE FILEPATH
    "Path to the profiling data file to use when compiling.")
  set(Scylla_PROFDATA_COMPRESSED_FILE
    "pgo/profiles/${CMAKE_SYSTEM_PROCESSOR}/profile.profdata.xz" CACHE FILEPATH
    "Path to the compressed profiling data file to use when compiling")
endif()

if(Scylla_PROFDATA_FILE)
  if(NOT EXISTS "${Scylla_PROFDATA_FILE}")
    message(FATAL_ERROR
      "Specified Scylla_PROFDATA_FILE (${Scylla_PROFDATA_FILE}) does not exist")
  endif()
  set(profdata_file "${Scylla_PROFDATA_FILE}")
endif()

if(Scylla_PROFDATA_COMPRESSED_FILE)
  # read the header to see if the file is fetched by LFS upon checkout
  file(READ "${Scylla_PROFDATA_COMPRESSED_FILE}" file_header LIMIT 7)
  if(file_header MATCHES "version")
    message(FATAL_ERROR "Please install git-lfs for using profdata stored in Git LFS")
  endif()
  get_filename_component(profdata_filename ${Scylla_PROFDATA_COMPRESSED_FILE} NAME_WLE)
  file(ARCHIVE_EXTRACT
    INPUT "${Scylla_PROFDATA_COMPRESSED_FILE}"
    DESTINATION "${CMAKE_BINARY_DIR}")
  set(profdata_file "${CMAKE_BINARY_DIR}/${prfdata_filename}")
endif()

if(Scylla_PROFDATA_FILE AND Scylla_PROFDATA_COMPRESSED_FILE)
  message(FATAL_ERROR
    "Both Scylla_PROFDATA_FILE and Scylla_PROFDATA_COMPRESSED_FILE are specified!")
endif()

if(profdata_file)
  # When building with PGO, -Wbackend-plugin generates a warning for every
  # function which changed its control flow graph since the profile was
  # taken.
  # We allow stale profiles, so these warnings are just noise to us.
  # Let's silence them.
  string(APPEND CMAKE_CXX_FLAGS " -Wno-backend-plugin")
  if(Scylla_BUILD_INSTRUMENTED MATCHES "IR|CSIR")
    string(APPEND CMAKE_CXX_FLAGS " -fprofile-use=\"${profdata_file}\"")
  else()
    string(APPEND CMAKE_CXX_FLAGS " -fprofile-instr-use=\"${profdata_file}\"")
  endif()
endif()
