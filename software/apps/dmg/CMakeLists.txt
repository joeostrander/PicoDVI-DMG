# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(dmg
    main.c
    analog_microphone.c
    emusound.c
    shared_dma_handler.c
    colors.c
    eeprom.c
    osd.c
    font_5x7.c
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(dmg PRIVATE -Wall)

# Optional Bluetooth controller support (Pico W / Pico 2 W).
# 0 = disabled (default), 1 = Bluetooth HID host.
set(USE_BLUETOOTH_CONTROLLER "0" CACHE STRING "Controller mode: 0=NES Classic/Wii I2C, 1=Bluetooth HID host")
set_property(CACHE USE_BLUETOOTH_CONTROLLER PROPERTY STRINGS 0 1)
message(STATUS "dmg: USE_BLUETOOTH_CONTROLLER=${USE_BLUETOOTH_CONTROLLER}")

set(BT_TEST_HOST_FILES_ROOT "${CMAKE_SOURCE_DIR}/ext/host_files")

if(USE_BLUETOOTH_CONTROLLER STREQUAL "1")
    set(BT_BLUEHOST_DIR "${BT_TEST_HOST_FILES_ROOT}/bluehost-lib")
    set(BT_PARSER_DIR "${BT_TEST_HOST_FILES_ROOT}/parser-lib")

    if(NOT EXISTS "${BT_BLUEHOST_DIR}/blue_host.c")
        message(FATAL_ERROR "Bluetooth host source not found: ${BT_BLUEHOST_DIR}/blue_host.c")
    endif()

    if(NOT EXISTS "${BT_PARSER_DIR}/gamepad_parser.c")
        message(FATAL_ERROR "Bluetooth parser source not found: ${BT_PARSER_DIR}/gamepad_parser.c")
    endif()

    target_sources(dmg PRIVATE
        ${BT_BLUEHOST_DIR}/blue_host.c
        ${BT_PARSER_DIR}/gamepad_parser.c
        ${BT_PARSER_DIR}/hidparser.c
    )

    target_include_directories(dmg PRIVATE
        ${BT_BLUEHOST_DIR}
        ${BT_PARSER_DIR}
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/include
    )
endif()

# Set resolution mode (choose 0/1/2)
# 0 = 640x480, horizontally scaled x4, vertically x3 --> 640x480 Full screen
# 1 = 800x600, horizontally scaled x4, vertically x4 --> 640x576 window
# 2 = 640x480, horizontally scaled x2, vertically x2 --> 320x288 window
set(RESOLUTION_MODE "0" CACHE STRING "Resolution mode: 0=640x480 x4/x3, 1=800x600 x4/x4, 2=640x480 x2/x2")
set_property(CACHE RESOLUTION_MODE PROPERTY STRINGS 0 1 2)

if(RESOLUTION_MODE STREQUAL "1")
    set(DVI_VERTICAL_REPEAT_VALUE 4)
elseif(RESOLUTION_MODE STREQUAL "2")
    set(DVI_VERTICAL_REPEAT_VALUE 2)
else()
    set(DVI_VERTICAL_REPEAT_VALUE 3)
endif()

target_compile_definitions(dmg PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=${DVI_VERTICAL_REPEAT_VALUE}
    DVI_SYMBOLS_PER_WORD=2
    RESOLUTION_MODE=${RESOLUTION_MODE}
    USE_BLUETOOTH_CONTROLLER=${USE_BLUETOOTH_CONTROLLER}
    PICO_FLASH_SIZE_BYTES=0x200000  # (2097152, 2MB) - I need to define this or it may set to 4MB by default
)

target_link_libraries(dmg
    pico_stdlib
    pico_stdio_uart
    pico_multicore
    pico_util
    libdvi
    libsprite
    hardware_pio
    hardware_i2c
    hardware_uart
    hardware_irq
    hardware_adc
    hardware_dma
    hardware_flash
)

if(USE_BLUETOOTH_CONTROLLER STREQUAL "1")
    target_link_libraries(dmg
        pico_btstack_classic
        pico_btstack_cyw43
        pico_cyw43_arch_none
    )
endif()

# Enable UART stdio and disable USB stdio
# Force printf to use UART0 instead of USB
# I'm setting these in main instead, before pico/stdlib.h is included
# set(PICO_DEFAULT_UART 1)         # Use UART1
# set(PICO_DEFAULT_UART_TX_PIN 20) # GPIO20 for TX
# set(PICO_DEFAULT_UART_RX_PIN 21) # GPIO21 for RX
pico_enable_stdio_uart(dmg 1)
pico_enable_stdio_usb(dmg 0)

# Build pio programs
pico_generate_pio_header(dmg ${CMAKE_CURRENT_LIST_DIR}/video_capture.pio)

target_include_directories(dmg PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# create map/bin/hex file etc.
pico_add_extra_outputs(dmg)
