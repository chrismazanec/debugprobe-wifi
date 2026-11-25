cmake_minimum_required(VERSION 3.12)

# build for the Raspberry Pi Debug Probe by default
set(PICO_BOARD debug_probe CACHE STRING "Board type")

set(CMAKE_BUILD_TYPE RelWithDebInfo)

include(pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(FreeRTOS_Kernel_import.cmake)

project(debugprobe)

pico_sdk_init()

add_executable(debugprobe
        src/probe_config.c
        src/main.c
        src/usb_descriptors.c
        src/probe.c
        src/cdc_uart.c
        src/get_serial.c
        src/sw_dp_pio.c
        src/tusb_edpt_handler.c
        src/autobaud.c
)

target_sources(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
        #CMSIS_DAP/CMSIS/DAP/Firmware/Source/SW_DP.c
        )

target_include_directories(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Include/
        CMSIS_DAP/CMSIS/Core/Include/
        include/
        )

target_compile_options(debugprobe PRIVATE -Wall)

pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe_oen.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/autobaud.pio)

target_include_directories(debugprobe PRIVATE src)

# add version
add_custom_target(version
        ${CMAKE_SOURCE_DIR}/get-version.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(debugprobe PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        )

add_dependencies(debugprobe version)

target_compile_definitions (debugprobe PRIVATE
	PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
)

if (NOT PICO_BOARD STREQUAL "debug_probe")
    set_target_properties(debugprobe PROPERTIES
        OUTPUT_NAME "debugprobe_on_${PICO_BOARD}"
    )
endif ()

target_link_libraries(debugprobe PRIVATE
        pico_multicore
        pico_stdlib
        pico_usb_reset
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_pio
        hardware_dma
        hardware_irq
        hardware_clocks
        FreeRTOS-Kernel
)

if (PICO_BOARD STREQUAL "pico2_w")
    # WiFi transport: CMSIS-DAP over TCP (OpenOCD "cmsis-dap tcp" backend).
    target_sources(debugprobe PRIVATE
            src/net/net_task.c
            src/net/netcfg.c
            src/net/netcfg_flash.c
            src/net/netcfg_console.c
            src/net/dap_tcp_proto.c
            src/net/dap_tcp_server.c
    )
    target_compile_definitions(debugprobe PRIVATE
            PROBE_NET_WIFI=1
            CYW43_TASK_STACK_SIZE=2048
            ASYNC_CONTEXT_DEFAULT_FREERTOS_TASK_STACK_SIZE=2048
            PICO_FLASH_ASSERT_ON_UNSAFE=0
    )
    target_link_libraries(debugprobe PRIVATE
            pico_cyw43_arch_lwip_sys_freertos
            pico_lwip_mdns
            pico_flash
            hardware_flash
            hardware_watchdog
            FreeRTOS-Kernel-Heap4
    )
    # lwIP's FreeRTOS port divides milliseconds by portTICK_PERIOD_MS, which
    # is 0 with debugprobe's 20 kHz tick (division by zero, sys_now() == 0).
    # Build a fixed copy instead of the SDK's sys_arch.c.
    get_target_property(_lwip_fr_srcs pico_lwip_contrib_freertos INTERFACE_SOURCES)
    list(FILTER _lwip_fr_srcs EXCLUDE REGEX "sys_arch\\.c$")
    set_target_properties(pico_lwip_contrib_freertos PROPERTIES INTERFACE_SOURCES "${_lwip_fr_srcs}")
    file(READ ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/sys_arch.c _sys_arch)
    string(REPLACE "delay_ms / portTICK_RATE_MS" "PROBE_MS_TO_TICKS(delay_ms)" _sys_arch "${_sys_arch}")
    string(REPLACE "timeout_ms / portTICK_RATE_MS" "PROBE_MS_TO_TICKS(timeout_ms)" _sys_arch "${_sys_arch}")
    string(REPLACE "return xTaskGetTickCount() * portTICK_PERIOD_MS;" "return (u32_t)(time_us_64() / 1000u);" _sys_arch "${_sys_arch}")
    string(FIND "${_sys_arch}" "portTICK_" _left)
    if (NOT _left EQUAL -1)
        message(FATAL_ERROR "lwIP sys_arch.c changed: tick conversion patch incomplete")
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/generated/lwip_sys_arch_fixed.c
        "/* generated from lwIP contrib/ports/freertos/sys_arch.c by CMakeLists.txt */\n"
        "#include <stdint.h>\n#include \"pico/time.h\"\n"
        "#define PROBE_MS_TO_TICKS(ms) ((TickType_t)((((uint64_t)(ms)) * configTICK_RATE_HZ + 999u) / 1000u))\n"
        "${_sys_arch}")
    target_sources(debugprobe PRIVATE ${CMAKE_BINARY_DIR}/generated/lwip_sys_arch_fixed.c)

    # The CYW43 firmware blob makes a RAM image too large; run from flash
    # (the SWD timing lives in PIO, so XIP does not matter).
else()
    target_link_libraries(debugprobe PRIVATE FreeRTOS-Kernel-Heap1)
    pico_set_binary_type(debugprobe copy_to_ram)
endif()

pico_add_extra_outputs(debugprobe)
