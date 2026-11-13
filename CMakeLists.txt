cmake_minimum_required(VERSION 3.18)
project(pt_stt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(_ptstt_default_backend metal)
else()
  set(_ptstt_default_backend cpu)
endif()
set(PTSTT_BACKEND "${_ptstt_default_backend}" CACHE STRING "Compute backend: cpu, metal, cuda or vulkan")
set_property(CACHE PTSTT_BACKEND PROPERTY STRINGS cpu metal cuda vulkan)
set(PTSTT_CUDA_ARCHITECTURES "87" CACHE STRING "CUDA architectures (87 = Jetson Orin)")
option(PTSTT_BUILD_CLI      "Build ptstt-cli"               ON)
option(PTSTT_BUILD_EXAMPLES "Build the C++ example"         ON)
option(PTSTT_BUILD_TESTS    "Build the ctest targets"       ON)
option(PTSTT_NATIVE         "Optimise for the build machine (off = portable binary)" ON)

set(PTSTT_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
set(PTSTT_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PTSTT_LIB_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PTSTT_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PTSTT_LIB_DIR}")

# ---------------------------------------------------------------------------
# parakeet.cpp (+ ggml) as an external project.
# Its CMakeLists uses CMAKE_SOURCE_DIR, so add_subdirectory() is not an option.
# Every shared library it builds is written straight into build/lib.
# ---------------------------------------------------------------------------
include(ExternalProject)

set(PARAKEET_SRC "${CMAKE_SOURCE_DIR}/third_party/parakeet.cpp")
if(NOT EXISTS "${PARAKEET_SRC}/CMakeLists.txt" OR NOT EXISTS "${PARAKEET_SRC}/third_party/ggml/CMakeLists.txt")
  message(FATAL_ERROR "third_party/parakeet.cpp is missing. Run: git submodule update --init --recursive")
endif()

set(_parakeet_args
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${PTSTT_LIB_DIR}
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${PTSTT_BIN_DIR}
  -DCMAKE_BUILD_RPATH=${PTSTT_LIB_DIR}
  -DPARAKEET_SHARED=ON
  -DPARAKEET_BUILD_CLI=ON
  -DPARAKEET_BUILD_SERVER=OFF
  -DPARAKEET_BUILD_TESTS=OFF
  -DPARAKEET_WITH_CED=OFF
  -DPARAKEET_WITH_VOICEDETECT=OFF
)
if(NOT PTSTT_NATIVE)
  list(APPEND _parakeet_args -DGGML_NATIVE=OFF)
endif()
if(PTSTT_BACKEND STREQUAL "metal")
  list(APPEND _parakeet_args -DPARAKEET_GGML_METAL=ON)
elseif(PTSTT_BACKEND STREQUAL "cuda")
  list(APPEND _parakeet_args -DPARAKEET_GGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=${PTSTT_CUDA_ARCHITECTURES})
elseif(PTSTT_BACKEND STREQUAL "vulkan")
  list(APPEND _parakeet_args -DPARAKEET_GGML_VULKAN=ON)
elseif(NOT PTSTT_BACKEND STREQUAL "cpu")
  message(FATAL_ERROR "PTSTT_BACKEND must be cpu, metal, cuda or vulkan (got '${PTSTT_BACKEND}')")
endif()
message(STATUS "pt-stt: backend=${PTSTT_BACKEND}")

set(_parakeet_lib "${PTSTT_LIB_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}parakeet${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(_ggml_base_lib "${PTSTT_LIB_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}ggml-base${CMAKE_SHARED_LIBRARY_SUFFIX}")
ExternalProject_Add(parakeet_ext
  SOURCE_DIR        "${PARAKEET_SRC}"
  BINARY_DIR        "${CMAKE_BINARY_DIR}/parakeet"
  CMAKE_ARGS        ${_parakeet_args}
  INSTALL_COMMAND   ""
  BUILD_BYPRODUCTS  "${_parakeet_lib}" "${_ggml_base_lib}"
  USES_TERMINAL_BUILD ON
)

add_library(parakeet SHARED IMPORTED GLOBAL)
set_target_properties(parakeet PROPERTIES
  IMPORTED_LOCATION "${_parakeet_lib}"
  INTERFACE_INCLUDE_DIRECTORIES "${PARAKEET_SRC}/include")
add_dependencies(parakeet parakeet_ext)

# ggml-base, only for ggml_log_set (to hide backend logs unless verbose).
add_library(ggml_base SHARED IMPORTED GLOBAL)
set_target_properties(ggml_base PROPERTIES
  IMPORTED_LOCATION "${_ggml_base_lib}"
  INTERFACE_INCLUDE_DIRECTORIES "${PARAKEET_SRC}/third_party/ggml/include")
add_dependencies(ggml_base parakeet_ext)

# ---------------------------------------------------------------------------
# libptstt
# ---------------------------------------------------------------------------
if(APPLE)
  set(_origin "@loader_path")
else()
  set(_origin "$ORIGIN")
endif()

add_library(ptstt SHARED
  src/engine.cpp
  src/audio_source.cpp
  src/miniaudio_impl.c
  src/json_util.cpp)
target_include_directories(ptstt
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(ptstt PRIVATE PTSTT_BUILDING_LIBRARY PTSTT_VERSION="${PROJECT_VERSION}")
set_target_properties(ptstt PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  BUILD_RPATH "${_origin}"
  INSTALL_RPATH "${_origin}")
find_package(Threads REQUIRED)
target_link_libraries(ptstt PRIVATE parakeet ggml_base Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(ptstt PRIVATE m)
endif()
if(APPLE)
  target_link_libraries(ptstt PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ptstt PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()

# Executables find libptstt/libparakeet in ../lib.
set(_exe_rpath "${_origin}/../lib")

if(PTSTT_BUILD_CLI)
  add_executable(ptstt-cli cli/main.cpp)
  target_link_libraries(ptstt-cli PRIVATE ptstt)
  set_target_properties(ptstt-cli PROPERTIES BUILD_RPATH "${_exe_rpath}" INSTALL_RPATH "${_exe_rpath}")
endif()

if(PTSTT_BUILD_EXAMPLES)
  add_executable(ptstt-example-listen examples/listen.cpp)
  target_link_libraries(ptstt-example-listen PRIVATE ptstt)
  set_target_properties(ptstt-example-listen PROPERTIES BUILD_RPATH "${_exe_rpath}" INSTALL_RPATH "${_exe_rpath}")
endif()

if(PTSTT_BUILD_TESTS)
  enable_testing()
  add_executable(test_engine tests/test_engine.cpp)
  target_link_libraries(test_engine PRIVATE ptstt)
  set_target_properties(test_engine PROPERTIES BUILD_RPATH "${_exe_rpath}")
  set(_models "${CMAKE_SOURCE_DIR}/models")
  add_test(NAME engine COMMAND test_engine
    --model "${_models}/pt-fastconformer-large.f32.gguf"
    --vad   "${_models}/silero-vad-f32.gguf"
    --fixtures "${CMAKE_SOURCE_DIR}/tests/fixtures")
  find_package(Python3 3.10 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME python COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_python.py")
    set_tests_properties(python PROPERTIES ENVIRONMENT
      "PTSTT_LIB=$<TARGET_FILE:ptstt>;PTSTT_MODELS_DIR=${_models};PYTHONPATH=${CMAKE_SOURCE_DIR}/python")
  endif()
endif()

# ---------------------------------------------------------------------------
# Install: lib/ (ptstt + parakeet + ggml), include/, bin/
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ptstt LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/ptstt.h include/ptstt.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${PTSTT_LIB_DIR}/" DESTINATION ${CMAKE_INSTALL_LIBDIR}
  FILES_MATCHING PATTERN "*parakeet*${CMAKE_SHARED_LIBRARY_SUFFIX}*" PATTERN "*ggml*${CMAKE_SHARED_LIBRARY_SUFFIX}*")
if(PTSTT_BUILD_CLI)
  install(TARGETS ptstt-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(PROGRAMS "${PTSTT_BIN_DIR}/parakeet-cli" DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
