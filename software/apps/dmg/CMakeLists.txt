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
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(dmg PRIVATE -Wall)

# Set resolution mode (change this to switch between modes)
set(RESOLUTION_800x600 0)  # Set to 1 for 800x600, 0 for 640x480

# Automatically set DVI_VERTICAL_REPEAT based on resolution
if(RESOLUTION_800x600)
    set(DVI_VERTICAL_REPEAT_VALUE 4)
else()
    set(DVI_VERTICAL_REPEAT_VALUE 3)
endif()

target_compile_definitions(dmg PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=${DVI_VERTICAL_REPEAT_VALUE}
    DVI_SYMBOLS_PER_WORD=2
    RESOLUTION_800x600=${RESOLUTION_800x600}
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
