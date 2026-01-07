# Fetch nlohmann/json
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(kv_store kv_store.cc)

target_link_libraries(kv_store
  PRIVATE
    pbft::core
    nlohmann_json::nlohmann_json
)
