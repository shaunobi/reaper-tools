include(FetchContent)

# pocketfft: header-only, vendored as a submodule (BSD 3-Clause).
add_library(pocketfft INTERFACE)
target_include_directories(pocketfft INTERFACE "${SRT_THIRD_PARTY}/pocketfft")
target_compile_definitions(pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING=1)
add_library(pocketfft::pocketfft ALIAS pocketfft)

# nlohmann/json (MIT) for model manifests and settings.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

if(SRT_BUILD_TESTS)
  # Catch2 (Boost Software License) for unit tests.
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.9.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
