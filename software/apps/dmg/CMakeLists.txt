# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(dmg
	main.c
)

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
)

# Build pio
pico_generate_pio_header(dmg ${CMAKE_CURRENT_LIST_DIR}/dmg.pio)


# create map/bin/hex file etc.
pico_add_extra_outputs(dmg)
