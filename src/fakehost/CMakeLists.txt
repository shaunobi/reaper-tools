# srt_fakehost: runs the real extension code against an in-memory stand-in
# for REAPER, so actions can be exercised and debugged without REAPER.
add_executable(srt_fakehost
  main.cpp
  fake_reaper.cpp
  progress_window_headless.cpp)
target_link_libraries(srt_fakehost PRIVATE reaper_srt_logic)
srt_keep_plugin_entry(srt_fakehost)
srt_copy_onnxruntime_next_to(srt_fakehost)
if(APPLE)
  set_target_properties(srt_fakehost PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(srt_fakehost PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

if(SRT_BUILD_TESTS)
  set(_clip "${CMAKE_SOURCE_DIR}/tests/data/beat/clip.wav")
  set(_mix "${CMAKE_SOURCE_DIR}/tests/data/sep/mix.wav")
  add_test(NAME fakehost.about COMMAND srt_fakehost about)
  add_test(NAME fakehost.fit COMMAND srt_fakehost fit "${_clip}" --bpm 120 --start-bar -2 --expect-bars 4)
  add_test(NAME fakehost.fit_no_leadin COMMAND srt_fakehost fit "${_clip}" --bpm 120 --start-bar 1 --expect-bars 4)
  add_test(NAME fakehost.conform COMMAND srt_fakehost conform "${_clip}" --bpm 120 --start-bar -2 --expect-bars 4)
  add_test(NAME fakehost.separate COMMAND srt_fakehost separate "${_mix}")
  set_tests_properties(fakehost.about fakehost.fit fakehost.fit_no_leadin fakehost.conform fakehost.separate
    PROPERTIES SKIP_RETURN_CODE 77 WORKING_DIRECTORY "$<TARGET_FILE_DIR:srt_fakehost>")
endif()
