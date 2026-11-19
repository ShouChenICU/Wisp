# The driver components (drivers/<chip>/) this board's board.cpp uses.
# firmware/CMakeLists.txt includes this file before project() and publishes
# the list as the WISP_BOARD_REQUIRES build property, which
# firmware/main/CMakeLists.txt appends to its REQUIRES.
set(WISP_BOARD_REQUIRES axp2101 pcf85063 qmi8658)
