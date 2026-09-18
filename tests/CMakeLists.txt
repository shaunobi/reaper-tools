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

add_executable(srt_tests
  test_wav.cpp
  test_fft.cpp
  test_jobs.cpp
  test_onnx.cpp
  test_resample.cpp
  test_mel.cpp
  test_onset.cpp
  test_beat_tracker.cpp
  test_beat_dbn.cpp)
target_link_libraries(srt_tests PRIVATE srt::core Catch2::Catch2WithMain)
target_compile_definitions(srt_tests PRIVATE
  SRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  SRT_BEAT_THIS_MODEL="${SRT_BEAT_THIS_MODEL}")
srt_copy_onnxruntime_next_to(srt_tests)
if(APPLE)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

include(Catch)
catch_discover_tests(srt_tests WORKING_DIRECTORY "$<TARGET_FILE_DIR:srt_tests>")
