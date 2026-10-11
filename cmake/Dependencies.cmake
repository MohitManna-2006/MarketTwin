include(FetchContent)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
get_target_property(_markettwin_json_includes nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nlohmann_json PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_markettwin_json_includes}")

# Pins are release tarballs with SHA256 hashes so a clean configure does not float to a new commit.
set(MARKETTWIN_GOOGLETEST_URL
    "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz"
)
set(MARKETTWIN_GOOGLETEST_SHA256
    "7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926"
)
set(MARKETTWIN_BENCHMARK_URL "https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz")
set(MARKETTWIN_BENCHMARK_SHA256
    "b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104"
)

if(MARKETTWIN_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL ${MARKETTWIN_GOOGLETEST_URL}
    URL_HASH SHA256=${MARKETTWIN_GOOGLETEST_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  # GTest::gtest is an alias. System includes belong on the real gtest target so -Werror
  # does not apply to third-party headers.
  if(TARGET gtest)
    get_target_property(_markettwin_gtest_includes gtest INTERFACE_INCLUDE_DIRECTORIES)
    if(_markettwin_gtest_includes)
      set_target_properties(
        gtest PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_markettwin_gtest_includes}"
      )
    endif()
    # gtest 1.15.2 warns on char8_t conversion under newer Apple Clang. Keep that out of our log.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(gtest PRIVATE -Wno-character-conversion)
    endif()
  endif()
endif()

if(MARKETTWIN_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    URL ${MARKETTWIN_BENCHMARK_URL}
    URL_HASH SHA256=${MARKETTWIN_BENCHMARK_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googlebenchmark)
endif()

if(MARKETTWIN_BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
  set(PYBIND11_INSTALL OFF CACHE BOOL "" FORCE)
  set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    pybind11
    URL "https://github.com/pybind/pybind11/archive/refs/tags/v3.0.1.tar.gz"
    URL_HASH SHA256=741633da746b7c738bb71f1854f957b9da660bcd2dce68d71949037f0969d0ca
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(pybind11)
endif()
