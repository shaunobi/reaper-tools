option(SRT_FETCH_TEST_MODELS "Download the models the tests need" ON)

# Downloads a model for the tests, verifies its SHA-256, and only warns when
# that fails (the tests that need it skip). file(DOWNLOAD)'s own
# EXPECTED_HASH is not used: on a failed download it raises a hard CMake
# error ("cannot compute hash on failed download") before our check runs.
# Extra arguments are passed to file(DOWNLOAD), e.g. HTTPHEADER lines.
function(srt_fetch_test_model url dest sha256 label)
  if(NOT SRT_FETCH_TEST_MODELS OR EXISTS "${dest}")
    return()
  endif()
  message(STATUS "Downloading the ${label} for tests")
  file(DOWNLOAD "${url}" "${dest}" STATUS _st SHOW_PROGRESS ${ARGN})
  list(GET _st 0 _code)
  if(_code EQUAL 0 AND EXISTS "${dest}")
    file(SHA256 "${dest}" _actual)
    if(NOT _actual STREQUAL "${sha256}")
      set(_st "checksum mismatch (got ${_actual})")
      set(_code 1)
    endif()
  endif()
  if(NOT _code EQUAL 0)
    file(REMOVE "${dest}")
    message(WARNING "Could not download the ${label}; the tests that need it will be skipped: ${_st}")
  endif()
endfunction()

# Beat This! final0, ONNX export (MIT). Used by the beat tracker correctness test.
set(SRT_BEAT_THIS_MODEL "${CMAKE_BINARY_DIR}/_models/beat-this-final0.onnx")
srt_fetch_test_model(
  "https://huggingface.co/aaatmy/beat-this-onnx/resolve/main/beat-this-final0.onnx"
  "${SRT_BEAT_THIS_MODEL}"
  e5e37b7d1802895e42559c5a1ced7619b1601d257a5c8e167c9d45de37c1b078
  "Beat This model")

# BS-RoFormer 4-stem, host-STFT ONNX export (MIT). Used by the separation correctness test.
set(SRT_SEPARATION_MODEL "${CMAKE_BINARY_DIR}/_models/bs_roformer_4stem_rhythm_fp16.onnx")
srt_fetch_test_model(
  "https://huggingface.co/silverdaw/bs-roformer-rhythm-onnx/resolve/main/bs_roformer_4stem_rhythm_fp16.onnx"
  "${SRT_SEPARATION_MODEL}"
  4bac38e42c085b5d69964e2b7ff161318a1315122c10b025c718b20e94d85d6b
  "BS-RoFormer model (257 MB)")

add_executable(srt_tests
  test_wav.cpp
  test_fft.cpp
  test_jobs.cpp
  test_onnx.cpp
  test_resample.cpp
  test_mel.cpp
  test_onset.cpp
  test_beat_tracker.cpp
  test_beat_dbn.cpp
  test_beat_gate.cpp
  test_grid_fit.cpp
  test_stft.cpp
  test_separator.cpp
  test_demucs.cpp
  test_sha256.cpp)
target_link_libraries(srt_tests PRIVATE srt::core Catch2::Catch2WithMain)
# Demucs v4 fine-tuned, four ONNX files exported by tools/export_demucs.py
# and published as release assets (see docs/BUILDING.md). The correctness
# test skips when they are missing.
set(SRT_DEMUCS_MODEL_DIR "${CMAKE_BINARY_DIR}/_models/demucs")
set(_demucs_base "https://github.com/shaunobi/reaper-tools/releases/download/models-v1")

# While the repository is private, its release assets can only be fetched
# through the GitHub API with a token (the plain download link answers 404).
# CI passes GITHUB_TOKEN; a developer can export one too. Behind a proxy
# that adds the credential itself, SRT_MODELS_VIA_API=ON takes the API route
# without a token.
option(SRT_MODELS_VIA_API "Fetch the Demucs test models through the GitHub API (needed while the repository is private)" OFF)
set(_demucs_token "$ENV{GITHUB_TOKEN}")
set(_demucs_release_json "")
if(SRT_FETCH_TEST_MODELS AND (_demucs_token OR SRT_MODELS_VIA_API))
  set(_auth "")
  if(_demucs_token)
    set(_auth HTTPHEADER "Authorization: Bearer ${_demucs_token}")
  endif()
  set(_release_file "${CMAKE_BINARY_DIR}/_models/demucs-release.json")
  file(DOWNLOAD "https://api.github.com/repos/shaunobi/reaper-tools/releases/tags/models-v1" "${_release_file}"
    HTTPHEADER "Accept: application/vnd.github+json" ${_auth} STATUS _st)
  list(GET _st 0 _code)
  if(_code EQUAL 0)
    file(READ "${_release_file}" _demucs_release_json)
  else()
    message(WARNING "Could not read the models-v1 release through the GitHub API: ${_st}")
  endif()
endif()

# Download URL and headers for one asset: the API route when the release
# listing was fetched, else the public link.
function(srt_demucs_asset name out_url out_headers)
  set(${out_url} "${_demucs_base}/${name}" PARENT_SCOPE)
  set(${out_headers} "" PARENT_SCOPE)
  if(NOT _demucs_release_json)
    return()
  endif()
  string(JSON _count LENGTH "${_demucs_release_json}" assets)
  if(_count EQUAL 0)
    return()
  endif()
  math(EXPR _last "${_count} - 1")
  foreach(_k RANGE 0 ${_last})
    string(JSON _n GET "${_demucs_release_json}" assets ${_k} name)
    if(_n STREQUAL "${name}")
      string(JSON _id GET "${_demucs_release_json}" assets ${_k} id)
      set(${out_url} "https://api.github.com/repos/shaunobi/reaper-tools/releases/assets/${_id}" PARENT_SCOPE)
      set(_h HTTPHEADER "Accept: application/octet-stream")
      if(_demucs_token)
        list(APPEND _h HTTPHEADER "Authorization: Bearer ${_demucs_token}")
      endif()
      set(${out_headers} "${_h}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()
set(_demucs_hashes
  drums  2a8ba51dc7aecc01703f080497b579534fe60f0cb54b3adb44e3892d68091b72
  bass   0eb1ab8a8402bf40c9a3ea78836ea76f5af972b9860d5fa12790abee55533876
  other  96e366848d855a5bba4fc725c90cd5f912fee75f05489f0571f8029f59283371
  vocals 23fca3d2ac4cce8a52bdca1586484423ebee8bf5d584788b21c3f85c383b0581)
list(LENGTH _demucs_hashes _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
  list(GET _demucs_hashes ${_i} _stem)
  math(EXPR _j "${_i} + 1")
  list(GET _demucs_hashes ${_j} _sha)
  srt_demucs_asset("htdemucs_ft_${_stem}.onnx" _url _headers)
  srt_fetch_test_model("${_url}" "${SRT_DEMUCS_MODEL_DIR}/htdemucs_ft_${_stem}.onnx" ${_sha}
    "Demucs ${_stem} model (174 MB)" ${_headers})
endforeach()
target_compile_definitions(srt_tests PRIVATE
  SRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  SRT_BEAT_THIS_MODEL="${SRT_BEAT_THIS_MODEL}"
  SRT_SEPARATION_MODEL="${SRT_SEPARATION_MODEL}"
  SRT_DEMUCS_MODEL_DIR="${SRT_DEMUCS_MODEL_DIR}")
srt_copy_onnxruntime_next_to(srt_tests)
if(APPLE)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

include(Catch)
catch_discover_tests(srt_tests WORKING_DIRECTORY "$<TARGET_FILE_DIR:srt_tests>")
