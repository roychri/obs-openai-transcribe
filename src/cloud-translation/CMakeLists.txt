# add source files
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE # ${CMAKE_CURRENT_SOURCE_DIR}/aws.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/azure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/claude.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/custom-api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/deepl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/google-cloud.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/openai.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/papago.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/translation-cloud.cpp
)
