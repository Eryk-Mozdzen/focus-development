cmake_minimum_required(VERSION 3.16)

set(CMAKE_TOOLCHAIN_FILE "toolchain.cmake")

project(bench)

enable_language(ASM C)

add_executable(${CMAKE_PROJECT_NAME}.elf
    "src/main.c"
    "src/fifo.c"
    "src/tmr.c"
    "src/bmp280_compensate.c"

    "cubemx/Core/Src/main.c"
    "cubemx/Core/Src/stm32u0xx_hal_msp.c"
    "cubemx/Core/Src/stm32u0xx_it.c"
    "cubemx/Core/Src/syscalls.c"
    "cubemx/Core/Src/sysmem.c"
    "cubemx/Core/Src/system_stm32u0xx.c"
    "cubemx/startup_stm32u083xx.s"

    "mongoose/mongoose.c"
    "mongoose-pppos/mg_pppos.c"

    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_adc.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_adc_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_comp.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_cortex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_crc.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_crc_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_cryp.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_cryp_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_dac.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_dac_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_dma.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_dma_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_exti.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_flash.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_flash_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_gpio.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_i2c.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_i2c_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_irda.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_iwdg.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_lcd.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_lptim.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_opamp.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_opamp_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_pcd.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_pcd_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_pwr.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_pwr_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rcc.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rcc_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rng.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rng_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rtc.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_rtc_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_smartcard.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_smartcard_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_spi.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_spi_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_tim.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_tim_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_tsc.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_uart.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_uart_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_usart.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_usart_ex.c"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Src/stm32u0xx_hal_wwdg.c"
)

target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE
	"src/include"
    "mongoose"
    "mongoose-pppos"
    "cubemx/Core/Inc"
    "cubemx/Drivers/STM32U0xx_HAL_Driver/Inc"
    "cubemx/Drivers/CMSIS/Include"
    "cubemx/Drivers/CMSIS/Device/ST/STM32U0xx/Include"
)

target_compile_definitions(${CMAKE_PROJECT_NAME}.elf PRIVATE
    USE_HAL_DRIVER
    STM32U083xx
)

target_compile_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:
        -mthumb
        -mcpu=cortex-m0
	>

	$<$<COMPILE_LANGUAGE:C>:
		-O2 -g
		-std=gnu23
		-Wall
        -Wextra
        -Wpedantic

		-ffunction-sections
		-fdata-sections

        -mthumb
		-mcpu=cortex-m0
	>
)

target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
	-T ${CMAKE_SOURCE_DIR}/cubemx/STM32U083xx_FLASH.ld
	-Wl,--gc-sections

    -mthumb
	-mcpu=cortex-m0
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_SIZE} ${CMAKE_PROJECT_NAME}.elf
)

include(embed.cmake)
target_embed_file(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/web/index.html")

add_custom_target(flash
    STM32_Programmer_CLI -c port=SWD -w ${CMAKE_PROJECT_NAME}.elf -rst
    DEPENDS ${CMAKE_PROJECT_NAME}.elf
)
