cmake_minimum_required(VERSION 3.21)
project(vulkan_water LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Zero-dependency build: native Win32 windowing, own math, own glTF loader.
# The only external requirement is the Vulkan SDK (loader + glslangValidator).
# Windows-only because the windowing layer uses the Win32 API directly.
if(NOT WIN32)
    message(FATAL_ERROR "vulkan-water is Windows-only (native Win32 windowing). Build with the Vulkan SDK on Windows + MSVC.")
endif()

if(MSVC)
    add_compile_options(/EHsc /MP)
endif()

find_package(Vulkan REQUIRED)

add_executable(water src/main.cpp)
set_target_properties(water PROPERTIES OUTPUT_NAME "water")
target_compile_definitions(water PRIVATE VK_USE_PLATFORM_WIN32_KHR _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(water PRIVATE Vulkan::Vulkan)   # gdi32/user32 come from the default Win32 set

# ---- shader compilation (GLSL -> SPIR-V) -----------------------------------
if(DEFINED Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    set(GLSL_COMPILER ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE})
else()
    find_program(GLSL_COMPILER NAMES glslangValidator
                 HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
endif()
if(NOT GLSL_COMPILER)
    message(FATAL_ERROR "glslangValidator not found. Install the Vulkan SDK and set VULKAN_SDK.")
endif()

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

file(GLOB SHADER_FILES ${SHADER_SRC_DIR}/*.vert ${SHADER_SRC_DIR}/*.frag)
set(SPV_OUTPUTS "")
foreach(SH ${SHADER_FILES})
    get_filename_component(FN ${SH} NAME)
    set(OUT ${SHADER_OUT_DIR}/${FN}.spv)
    add_custom_command(
        OUTPUT  ${OUT}
        COMMAND ${GLSL_COMPILER} --target-env vulkan1.2 -I${SHADER_SRC_DIR} ${SH} -o ${OUT}
        DEPENDS ${SH} ${SHADER_SRC_DIR}/common.glsl ${SHADER_SRC_DIR}/shading.glsl
        COMMENT "Compiling ${FN}")
    list(APPEND SPV_OUTPUTS ${OUT})
endforeach()

add_custom_target(shaders DEPENDS ${SPV_OUTPUTS})
add_dependencies(water shaders)
target_compile_definitions(water PRIVATE SHADER_DIR="${SHADER_OUT_DIR}")
