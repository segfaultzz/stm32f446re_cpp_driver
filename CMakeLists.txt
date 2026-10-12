cmake_minimum_required(VERSION 3.28.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(app LANGUAGES ASM C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
    cmsis_core
    GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
    GIT_TAG        5.9.0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    cmsis_device_f4
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis_device_f4.git
    GIT_TAG        v2.6.8
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(cmsis_core cmsis_device_f4)

add_library(cmsis INTERFACE)

target_include_directories(cmsis INTERFACE
    ${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include
    ${cmsis_device_f4_SOURCE_DIR}/Include
)

target_compile_definitions(cmsis INTERFACE
    STM32F446xx
)


add_executable(${PROJECT_NAME})

# CMAKE_EXECUTABLE_SUFFIX from the toolchain file is reset by CMake's Generic platform, so set it here
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")

target_sources(${PROJECT_NAME} PRIVATE
    src/main.cc
    src/syscalls.c
    ${cmsis_device_f4_SOURCE_DIR}/Source/Templates/gcc/startup_stm32f446xx.s
    ${cmsis_device_f4_SOURCE_DIR}/Source/Templates/system_stm32f4xx.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ./inc
)

# Link CMSIS headers and definitions to the app
target_link_libraries(${PROJECT_NAME} PRIVATE
    cmsis
)

# ==============================================================================
# 4. Cortex-M4 Architecture & Linker Settings
# ==============================================================================
target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -ffunction-sections
    -fdata-sections
    -g
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Debug>:-Og>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -T${CMAKE_CURRENT_SOURCE_DIR}/STM32F446RETx_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map
    --specs=nosys.specs
    --specs=nano.specs
)
