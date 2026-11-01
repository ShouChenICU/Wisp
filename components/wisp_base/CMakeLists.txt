wisp_component(
    NAME wisp_base
    SRCS src/error.cpp src/value.cpp src/json.cpp
    INCLUDE_DIRS include
    IDF_REQUIRES bblanchon__arduinojson
    HOST_LIBS ArduinoJson
)
