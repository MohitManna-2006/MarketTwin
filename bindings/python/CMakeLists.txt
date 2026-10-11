# markettwin._native: the run-level boundary between the C++ core and Python.
# Built only with -DMARKETTWIN_BUILD_PYTHON=ON, normally through python/pyproject.toml.

pybind11_add_module(markettwin_native MODULE markettwin_bindings.cpp)
set_target_properties(markettwin_native PROPERTIES OUTPUT_NAME "_native")
target_link_libraries(markettwin_native PRIVATE markettwin::core)
target_compile_features(markettwin_native PRIVATE cxx_std_20)

execute_process(COMMAND git describe --always --dirty
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  OUTPUT_VARIABLE MARKETTWIN_BINDING_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT MARKETTWIN_BINDING_REVISION)
  set(MARKETTWIN_BINDING_REVISION "unknown")
endif()
target_compile_definitions(markettwin_native PRIVATE
  "MARKETTWIN_GIT_REVISION=\"${MARKETTWIN_BINDING_REVISION}\""
  "MARKETTWIN_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
  "MARKETTWIN_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")

markettwin_enable_sanitizers(markettwin_native)

# scikit-build-core installs into the wheel root; the module lives inside the package.
install(TARGETS markettwin_native LIBRARY DESTINATION markettwin)
