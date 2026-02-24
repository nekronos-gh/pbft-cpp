# Generate integration tests
add_executable(integration_test integration.cc)
target_link_libraries(integration_test 
    PRIVATE 
    pbft::core
)
