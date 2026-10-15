# Fetches a prebuilt ONNX Runtime for the current platform and exposes it as
# the imported target onnxruntime::onnxruntime. Also records the shared
# libraries that must ship next to the extension in SRT_ONNXRUNTIME_RUNTIME_FILES.
#
# Windows: the Microsoft.ML.OnnxRuntime.DirectML NuGet package (contains the
#          DirectML execution provider) plus Microsoft.AI.DirectML for DirectML.dll.
# macOS:   the official onnxruntime-osx-arm64 release archive (CoreML included).
# Linux:   the official onnxruntime-linux-x64 release archive (CPU only).

set(SRT_ORT_VERSION "1.24.4" CACHE STRING "ONNX Runtime version to fetch")
set(SRT_DIRECTML_VERSION "1.15.4" CACHE STRING "Microsoft.AI.DirectML version to fetch")
set(SRT_ORT_ROOT "${CMAKE_BINARY_DIR}/_deps/onnxruntime" CACHE PATH "Where ONNX Runtime is unpacked")

set(_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${SRT_ORT_VERSION}")

function(_srt_fetch_archive url dest_dir marker sha256)
  if(EXISTS "${dest_dir}/${marker}")
    return()
  endif()
  get_filename_component(_name "${url}" NAME)
  string(REGEX REPLACE "[^A-Za-z0-9._-]" "_" _name "${_name}")
  set(_archive "${CMAKE_BINARY_DIR}/_downloads/${_name}")
  if(NOT EXISTS "${_archive}")
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${_archive}" SHOW_PROGRESS STATUS _st EXPECTED_HASH SHA256=${sha256})
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_archive}")
      message(FATAL_ERROR "Download failed: ${url}: ${_st}")
    endif()
  endif()
  file(MAKE_DIRECTORY "${dest_dir}")
  file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${dest_dir}")
  if(NOT EXISTS "${dest_dir}/${marker}")
    message(FATAL_ERROR "Archive ${url} did not contain expected file ${marker}")
  endif()
endfunction()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set(SRT_ONNXRUNTIME_RUNTIME_FILES "")
set(SRT_ONNXRUNTIME_DEFINES "")

if(WIN32)
  set(_dir "${SRT_ORT_ROOT}/win-x64")
  _srt_fetch_archive(
    "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/${SRT_ORT_VERSION}"
    "${_dir}" "build/native/include/onnxruntime_c_api.h"
    "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681")
  set(_dml "${SRT_ORT_ROOT}/directml")
  _srt_fetch_archive(
    "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${SRT_DIRECTML_VERSION}"
    "${_dml}" "bin/x64-win/DirectML.dll"
    "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9")
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_dir}/runtimes/win-x64/native/onnxruntime.dll"
    IMPORTED_IMPLIB "${_dir}/runtimes/win-x64/native/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_dir}/build/native/include")
  list(APPEND SRT_ONNXRUNTIME_RUNTIME_FILES
    "${_dir}/runtimes/win-x64/native/onnxruntime.dll"
    "${_dir}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
    "${_dml}/bin/x64-win/DirectML.dll")
  list(APPEND SRT_ONNXRUNTIME_DEFINES SRT_HAVE_DML=1)
  if(NOT MSVC)
    # The MinGW cross-build has no Windows SDK: take DirectML.h from the
    # DirectML package, and stub the one source annotation MinGW's sal.h lacks.
    set_property(TARGET onnxruntime::onnxruntime APPEND PROPERTY
      INTERFACE_INCLUDE_DIRECTORIES "${_dml}/include")
    list(APPEND SRT_ONNXRUNTIME_DEFINES "_Maybenull_=")
  endif()
elseif(APPLE)
  set(_dir "${SRT_ORT_ROOT}/osx-arm64")
  _srt_fetch_archive(
    "${_ort_base}/onnxruntime-osx-arm64-${SRT_ORT_VERSION}.tgz"
    "${_dir}" "onnxruntime-osx-arm64-${SRT_ORT_VERSION}/include/onnxruntime_c_api.h"
    "93787795f47e1eee369182e43ed51b9e5da0878ab0346aecf4258979b8bba989")
  set(_dir "${_dir}/onnxruntime-osx-arm64-${SRT_ORT_VERSION}")
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_dir}/lib/libonnxruntime.${SRT_ORT_VERSION}.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${_dir}/include")
  list(APPEND SRT_ONNXRUNTIME_RUNTIME_FILES "${_dir}/lib/libonnxruntime.${SRT_ORT_VERSION}.dylib")
  list(APPEND SRT_ONNXRUNTIME_DEFINES SRT_HAVE_COREML=1)
else()
  set(_dir "${SRT_ORT_ROOT}/linux-x64")
  _srt_fetch_archive(
    "${_ort_base}/onnxruntime-linux-x64-${SRT_ORT_VERSION}.tgz"
    "${_dir}" "onnxruntime-linux-x64-${SRT_ORT_VERSION}/include/onnxruntime_c_api.h"
    "3a211fbea252c1e66290658f1b735b772056149f28321e71c308942cdb54b747")
  set(_dir "${_dir}/onnxruntime-linux-x64-${SRT_ORT_VERSION}")
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${_dir}/lib/libonnxruntime.so.${SRT_ORT_VERSION}"
    INTERFACE_INCLUDE_DIRECTORIES "${_dir}/include")
  list(APPEND SRT_ONNXRUNTIME_RUNTIME_FILES
    "${_dir}/lib/libonnxruntime.so.${SRT_ORT_VERSION}"
    "${_dir}/lib/libonnxruntime.so.1")
endif()

set_property(TARGET onnxruntime::onnxruntime APPEND PROPERTY
  INTERFACE_COMPILE_DEFINITIONS ${SRT_ONNXRUNTIME_DEFINES})

# Copies the ONNX Runtime shared libraries next to a target's output.
function(srt_copy_onnxruntime_next_to target)
  foreach(f IN LISTS SRT_ONNXRUNTIME_RUNTIME_FILES)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endforeach()
endfunction()
