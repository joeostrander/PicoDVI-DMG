# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(dmg
    main.c
    analog_microphone.c
    emusound.c
)

target_compile_options(dmg PRIVATE -Wall)

target_compile_definitions(dmg PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=4
    DVI_SYMBOLS_PER_WORD=2
)

target_link_libraries(dmg
    pico_stdlib
    pico_multicore
    pico_util
    libdvi
    libsprite
    hardware_pio
    hardware_i2c
    hardware_irq
    hardware_adc
    hardware_dma
)

# Enable USB serial output for printf debugging
pico_enable_stdio_usb(dmg 1)
pico_enable_stdio_uart(dmg 0)

# Build pio programs
pico_generate_pio_header(dmg ${CMAKE_CURRENT_LIST_DIR}/video_capture.pio)
pico_generate_pio_header(dmg ${CMAKE_CURRENT_LIST_DIR}/dmg_buttons.pio)


# create map/bin/hex file etc.
pico_add_extra_outputs(dmg)
