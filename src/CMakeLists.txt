# --- core: logging and paths (no third-party deps) ---
add_library(lantern_core STATIC core/log.cpp core/paths.cpp)
target_include_directories(lantern_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lantern_core PRIVATE lantern_warnings)

# --- input: device-agnostic mapping (no SDL) ---
add_library(lantern_input STATIC input/pad.cpp input/binding.cpp input/deadzone.cpp input/input_system.cpp)
target_link_libraries(lantern_input PUBLIC lantern_core PRIVATE lantern_warnings)

# --- config: TOML settings ---
add_library(lantern_config STATIC config/config.cpp)
target_link_libraries(lantern_config PUBLIC lantern_input PRIVATE tomlplusplus::tomlplusplus lantern_warnings)

# --- render: abstraction + null + vulkan backends ---
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/render/vulkan/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(shader placeholder.vert placeholder.frag)
  set(out ${SHADER_OUT}/${shader}.spv.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -o ${out} ${SHADER_DIR}/${shader}
    DEPENDS ${SHADER_DIR}/${shader}
    COMMENT "glslc ${shader}"
    VERBATIM)
  list(APPEND SHADER_INCS ${out})
endforeach()
add_custom_target(lantern_shaders DEPENDS ${SHADER_INCS})

add_library(lantern_render STATIC render/null_renderer.cpp render/vulkan/vk_renderer.cpp)
add_dependencies(lantern_render lantern_shaders)
# Vulkan structs are initialised as T{sType}; remaining members are value-initialised.
set_source_files_properties(render/vulkan/vk_renderer.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
target_include_directories(lantern_render PRIVATE ${SHADER_OUT})
target_link_libraries(lantern_render PUBLIC lantern_config PRIVATE Vulkan::Vulkan
                      lantern_warnings)

# --- platform: SDL3 window/events ---
add_library(lantern_platform STATIC platform/sdl_platform.cpp)
target_link_libraries(lantern_platform PUBLIC lantern_render lantern_input PRIVATE SDL3::SDL3 lantern_warnings)

# --- app ---
add_library(lantern_cli STATIC app/cli.cpp)
target_link_libraries(lantern_cli PUBLIC lantern_config PRIVATE lantern_warnings)

add_executable(lantern app/main.cpp)
target_compile_definitions(lantern PRIVATE LANTERN_VERSION="${PROJECT_VERSION}")
target_link_libraries(lantern PRIVATE lantern_cli lantern_platform lantern_render lantern_config SDL3::SDL3
                      lantern_warnings)
