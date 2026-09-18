configure_file(include/srt/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/srt/version.h" @ONLY)

add_library(srt_core STATIC
  src/wav.cpp
  src/fft.cpp
  src/log.cpp
  src/onnx.cpp
  src/jobs.cpp
  src/resample.cpp
  src/mel.cpp
  src/beat_tracker.cpp
  src/onset.cpp
  src/beat_dbn.cpp
  src/beat_detect.cpp
  src/grid_fit.cpp
)
add_library(srt::core ALIAS srt_core)

target_include_directories(srt_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(srt_core
  PUBLIC onnxruntime::onnxruntime nlohmann_json::nlohmann_json
  PRIVATE pocketfft::pocketfft r8brain::r8brain)

find_package(Threads REQUIRED)
target_link_libraries(srt_core PUBLIC Threads::Threads)

# Embed the smoke model so the extension can self-test without any file on disk.
set(_smoke_header "${CMAKE_CURRENT_BINARY_DIR}/generated/srt/smoke_model.h")
add_custom_command(
  OUTPUT "${_smoke_header}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/data/smoke.onnx
    -DOUTPUT=${_smoke_header}
    -DSYMBOL=kSrtSmokeModel
    -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
  DEPENDS "${CMAKE_SOURCE_DIR}/tests/data/smoke.onnx" "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
  COMMENT "Embedding smoke.onnx"
  VERBATIM)
target_sources(srt_core PRIVATE "${_smoke_header}")
