# The REAPER extension: reaper_srt.dll / reaper_srt.dylib / reaper_srt.so

# Everything except the window code goes into a static library so the fake
# host (src/fakehost) can run the same code outside REAPER.
add_library(reaper_srt_logic STATIC
  entry.cpp
  reaper_api.cpp
  actions.cpp
  action_about.cpp
  action_drums.cpp
  action_separate.cpp
  item_audio.cpp
  timeline.cpp
  models.cpp
  host.cpp
  runtime_loader.cpp
  crash_guard.cpp)
target_link_libraries(reaper_srt_logic PUBLIC srt::core)
target_include_directories(reaper_srt_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(reaper_srt_logic SYSTEM PUBLIC
  "${SRT_REAPER_SDK_DIR}"
  "${SRT_WDL_DIR}")
if(NOT WIN32)
  target_compile_definitions(reaper_srt_logic PUBLIC SWELL_PROVIDED_BY_APP=1)
endif()

# Forces the linker to pull ReaperPluginEntry out of reaper_srt_logic. The
# symbol is extern "C": undecorated on 64-bit Windows and Linux, with a
# leading underscore on macOS.
function(srt_keep_plugin_entry target)
  if(MSVC)
    target_link_options(${target} PRIVATE "/INCLUDE:ReaperPluginEntry")
  elseif(APPLE)
    target_link_options(${target} PRIVATE "-Wl,-u,_ReaperPluginEntry")
  else()
    target_link_options(${target} PRIVATE "-Wl,-u,ReaperPluginEntry")
  endif()
endfunction()

set(_sources progress_window.cpp)

if(WIN32)
  list(APPEND _sources resource.rc)
else()
  # SWELL: REAPER provides the implementation; the stub forwards to it.
  if(APPLE)
    set(_swell_stub "${SRT_WDL_DIR}/swell/swell-modstub.mm")
  else()
    set(_swell_stub "${SRT_WDL_DIR}/swell/swell-modstub-generic.cpp")
  endif()
  set_source_files_properties("${_swell_stub}" PROPERTIES COMPILE_OPTIONS "-w")
  list(APPEND _sources "${_swell_stub}")
  # Convert the Win32 dialog resource into SWELL's table form.
  find_program(PERL_EXECUTABLE perl REQUIRED)
  set(_rc_dir "${CMAKE_CURRENT_BINARY_DIR}/swell_res")
  add_custom_command(
    OUTPUT "${_rc_dir}/resource.rc_mac_dlg"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_rc_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc" "${_rc_dir}/resource.rc"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resource.h" "${_rc_dir}/resource.h"
    COMMAND ${PERL_EXECUTABLE} "${SRT_WDL_DIR}/swell/swell_resgen.pl" --force "${_rc_dir}/resource.rc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc" "${CMAKE_CURRENT_SOURCE_DIR}/resource.h"
    COMMENT "Generating SWELL dialog resources"
    VERBATIM)
  list(APPEND _sources "${_rc_dir}/resource.rc_mac_dlg")
endif()

add_library(reaper_srt MODULE ${_sources})
target_link_libraries(reaper_srt PRIVATE reaper_srt_logic)
# The entry point lives in the static library; make the linker keep it.
srt_keep_plugin_entry(reaper_srt)

set_target_properties(reaper_srt PROPERTIES PREFIX "" OUTPUT_NAME "reaper_srt")
if(APPLE)
  set_target_properties(reaper_srt PROPERTIES SUFFIX ".dylib" BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
  target_link_libraries(reaper_srt PRIVATE "-framework Cocoa")
elseif(UNIX)
  set_target_properties(reaper_srt PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

if(NOT WIN32)
  target_include_directories(reaper_srt PRIVATE "${_rc_dir}")
  set_source_files_properties("${_rc_dir}/resource.rc_mac_dlg" PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(MSVC)
  # Delay-load the runtime so the extension can bind it to the copy next to
  # itself at load time instead of whatever Windows finds first (see
  # runtime_loader.h).
  target_link_options(reaper_srt PRIVATE "/DELAYLOAD:onnxruntime.dll")
  target_link_libraries(reaper_srt PRIVATE delayimp)
elseif(WIN32)
  # Same thing with lld (the MinGW cross-build); the helper lives in libmingwex.
  target_link_options(reaper_srt PRIVATE "-Wl,--delayload=onnxruntime.dll")
endif()
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
  # __try/__except needs the Microsoft extensions switched on under clang+MinGW.
  set_source_files_properties(crash_guard.cpp PROPERTIES COMPILE_OPTIONS "-fms-extensions")
endif()

srt_copy_onnxruntime_next_to(reaper_srt)

# `cmake --install <build> --prefix <dir>` produces a ready-to-copy UserPlugins folder.
# The runtime libraries are installed from the copies placed next to the
# extension at build time, so symlinks in the upstream package become real files.
install(TARGETS reaper_srt LIBRARY DESTINATION UserPlugins)
foreach(_f IN LISTS SRT_ONNXRUNTIME_RUNTIME_FILES)
  get_filename_component(_name "${_f}" NAME)
  install(FILES "$<TARGET_FILE_DIR:reaper_srt>/${_name}" DESTINATION UserPlugins)
endforeach()
