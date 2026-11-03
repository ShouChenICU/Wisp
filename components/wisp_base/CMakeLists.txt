# Self-contained include (see cmake/wisp_component.cmake's header comment
# on why every wisp_* component's CMakeLists.txt needs this, not just
# some top-level CMakeLists.txt).
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_base
    SRCS src/error.cpp src/value.cpp src/json.cpp src/sha256.cpp src/base64.cpp
    INCLUDE_DIRS include
    IDF_REQUIRES bblanchon__arduinojson
    HOST_LIBS ArduinoJson
)
