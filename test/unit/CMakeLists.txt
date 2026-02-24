include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
enable_testing()

# Generate unitary tests
add_executable(unit_tests 
  client_ut.cc
  node_ut.cc
)
target_link_libraries(unit_tests 
    PRIVATE 
    pbft::core
    GTest::gtest 
    GTest::gtest_main 
    GTest::gmock
)

# Register with CTest
gtest_discover_tests(unit_tests)
