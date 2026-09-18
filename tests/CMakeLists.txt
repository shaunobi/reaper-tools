add_executable(srt_tests
  test_wav.cpp
  test_fft.cpp
  test_jobs.cpp
  test_onnx.cpp)
target_link_libraries(srt_tests PRIVATE srt::core Catch2::Catch2WithMain)
target_compile_definitions(srt_tests PRIVATE SRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
srt_copy_onnxruntime_next_to(srt_tests)
if(APPLE)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(srt_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

include(Catch)
catch_discover_tests(srt_tests WORKING_DIRECTORY "$<TARGET_FILE_DIR:srt_tests>")
