cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

include("cmake/gcc-arm-none-eabi.cmake")
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
enable_language(C ASM)

project(32demo)
message("Build type: " ${CMAKE_BUILD_TYPE})

# ═══════════════ 共享的 HAL 库 ═══════════════
add_subdirectory(cmake/stm32cubemx)

# ═══════════════ Bootloader 固件 ═══════════════
# Flash: 0x08000000  32KB
add_executable(32demo_boot
    Core/Src/bootloader.c
    Core/Src/ota.c
)

target_link_libraries(32demo_boot PRIVATE stm32cubemx)

target_link_options(32demo_boot PRIVATE
    -T${CMAKE_SOURCE_DIR}/stm32g431rbtx_boot.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/32demo_boot.map
    --specs=nosys.specs
    --specs=nano.specs
)

# ═══════════════ App 固件 ═══════════════
# Flash: 0x08008000  96KB
add_executable(32demo_app
    Core/Src/main.c
    Core/Src/stm32g4xx_it.c
    Core/Src/stm32g4xx_hal_msp.c
)

target_link_libraries(32demo_app PRIVATE stm32cubemx)

target_link_options(32demo_app PRIVATE
    -T${CMAKE_SOURCE_DIR}/stm32g431rbtx_app.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/32demo_app.map
    --specs=nosys.specs
    --specs=nano.specs
)
