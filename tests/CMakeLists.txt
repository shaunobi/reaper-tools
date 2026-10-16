option(SRT_FETCH_TEST_MODELS "Download the models the tests need" ON)

# Beat This! final0, ONNX export (MIT). Used by the beat tracker correctness test.
set(SRT_BEAT_THIS_MODEL "${CMAKE_BINARY_DIR}/_models/beat-this-final0.onnx")
if(SRT_FETCH_TEST_MODELS AND NOT EXISTS "${SRT_BEAT_THIS_MODEL}")
  message(STATUS "Downloading Beat This model for tests")
  file(DOWNLOAD
    "https://huggingface.co/aaatmy/beat-this-onnx/resolve/main/beat-this-final0.onnx"
    "${SRT_BEAT_THIS_MODEL}"
    EXPECTED_HASH SHA256=e5e37b7d1802895e42559c5a1ced7619b1601d257a5c8e167c9d45de37c1b078
    STATUS _st SHOW_PROGRESS)
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    file(REMOVE "${SRT_BEAT_THIS_MODEL}")
    message(WARNING "Could not download the Beat This model; its tests will be skipped: ${_st}")
  endif()
endif()

# BS-RoFormer 4-stem, host-STFT ONNX export (MIT). Used by the separation correctness test.
set(SRT_SEPARATION_MODEL "${CMAKE_BINARY_DIR}/_models/bs_roformer_4stem_rhythm_fp16.onnx")
if(SRT_FETCH_TEST_MODELS AND NOT EXISTS "${SRT_SEPARATION_MODEL}")
  message(STATUS "Downloading BS-RoFormer model for tests (257 MB)")
  file(DOWNLOAD
    "https://huggingface.co/silverdaw/bs-roformer-rhythm-onnx/resolve/main/bs_roformer_4stem_rhythm_fp16.onnx"
    "${SRT_SEPARATION_MODEL}"
    EXPECTED_HASH SHA256=4bac38e42c085b5d69964e2b7ff161318a1315122c10b025c718b20e94d85d6b
    STATUS _st SHOW_PROGRESS)
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    file(REMOVE "${SRT_SEPARATION_MODEL}")
    message(WARNING "Could not download the BS-RoFormer model; its test will be skipped: ${_st}")
  endif()
endif()

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
  set(_file "${SRT_DEMUCS_MODEL_DIR}/htdemucs_ft_${_stem}.onnx")
  if(SRT_FETCH_TEST_MODELS AND NOT EXISTS "${_file}")
    message(STATUS "Downloading Demucs ${_stem} model for tests (174 MB)")
    file(DOWNLOAD "${_demucs_base}/htdemucs_ft_${_stem}.onnx" "${_file}"
      EXPECTED_HASH SHA256=${_sha} STATUS _st SHOW_PROGRESS)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_file}")
      message(WARNING "Could not download the Demucs ${_stem} model; its test will be skipped: ${_st}")
    endif()
  endif()
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
