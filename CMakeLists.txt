cmake_minimum_required(VERSION 3.20)
project(systemc_accelerator_model CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ACCEL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ACCEL_TESTS "Build the GoogleTest suite" ON)

# SystemC 3.x (built from source; point CMAKE_PREFIX_PATH or SystemC_ROOT at the install)
find_package(SystemCLanguage CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

if(ACCEL_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(accel src/model.cpp src/tile.cpp)
target_include_directories(accel PUBLIC include)
target_compile_options(accel PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(accel PUBLIC SystemC::systemc nlohmann_json::nlohmann_json)

add_executable(accel_sim src/main.cpp)
target_link_libraries(accel_sim PRIVATE accel)
target_compile_options(accel_sim PRIVATE -Wall -Wextra -Wpedantic)

if(ACCEL_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(accel_tests tests/test_model.cpp tests/test_tile.cpp tests/test_main.cpp)
  target_link_libraries(accel_tests PRIVATE accel GTest::gtest)
  target_compile_options(accel_tests PRIVATE -Wall -Wextra)
  target_compile_definitions(accel_tests PRIVATE ACCEL_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  include(GoogleTest)
  gtest_discover_tests(accel_tests DISCOVERY_TIMEOUT 60)
endif()
