include(FetchContent)

FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)

FetchContent_MakeAvailable(nlohmann_json)

# Add include directories
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
