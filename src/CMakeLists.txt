# --- core: logging and paths (no third-party deps) ---
add_library(lantern_core STATIC core/log.cpp core/paths.cpp core/sha256.cpp core/png.cpp)
target_include_directories(lantern_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lantern_core PRIVATE lantern_warnings)

# --- iso + ingest: read-only disc access, identification, extraction ---
add_library(lantern_iso STATIC iso/iso9660.cpp)
target_link_libraries(lantern_iso PUBLIC lantern_core PRIVATE lantern_warnings)
add_library(lantern_ingest STATIC ingest/disc.cpp)
target_link_libraries(lantern_ingest PUBLIC lantern_iso PRIVATE lantern_warnings)
add_executable(lantern-ingest ingest/ingest_main.cpp)
target_link_libraries(lantern-ingest PRIVATE lantern_ingest lantern_warnings)

# --- ee: R5900 decoding, ELF loading (shared by analysis tools and the recompiler) ---
add_library(lantern_ee STATIC ee/elf.cpp ee/r5900.cpp ee/interpreter.cpp)
target_link_libraries(lantern_ee PUBLIC lantern_core PRIVATE lantern_warnings)
add_executable(lantern-disasm ee/disasm_main.cpp)
target_link_libraries(lantern-disasm PRIVATE lantern_ee lantern_warnings)

# --- analysis: static inventory of EE code, overlays, IOP modules ---
add_library(lantern_analysis STATIC analysis/code_analysis.cpp analysis/iop.cpp analysis/vif.cpp
            analysis/ps2_syscalls.cpp)
target_link_libraries(lantern_analysis PUBLIC lantern_ee PRIVATE lantern_warnings)
add_executable(lantern-analyze analysis/analyze_main.cpp)
target_link_libraries(lantern-analyze PRIVATE lantern_analysis lantern_iso lantern_warnings)

# --- recomp: C++ emitter + runtime dispatcher ---
add_library(lantern_recomp_runtime STATIC recomp/runtime.cpp)
target_link_libraries(lantern_recomp_runtime PUBLIC lantern_ee PRIVATE lantern_warnings)
add_library(lantern_recomp STATIC recomp/emitter.cpp)
target_link_libraries(lantern_recomp PUBLIC lantern_analysis lantern_recomp_runtime PRIVATE lantern_warnings)
add_executable(lantern-recomp recomp/recomp_main.cpp)
target_compile_definitions(lantern-recomp PRIVATE LANTERN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(lantern-recomp PRIVATE lantern_recomp lantern_iso lantern_warnings)

# --- ps2: HLE kernel, EE hardware registers, boot ---
add_library(lantern_gs STATIC gs/gs_memory.cpp gs/gs.cpp gs/rasterizer.cpp)
target_link_libraries(lantern_gs PUBLIC lantern_core PRIVATE lantern_warnings)
# --- iop: R3000A core (IOP CPU) ---
add_library(lantern_iop STATIC iop/r3000.cpp iop/iop_interp.cpp iop/irx.cpp iop/hle.cpp)
target_link_libraries(lantern_iop PUBLIC lantern_core lantern_ee PRIVATE lantern_warnings)
add_executable(lantern-iop iop/iop_main.cpp)
target_link_libraries(lantern-iop PRIVATE lantern_iop lantern_warnings)

add_library(lantern_ps2 STATIC ps2/hardware.cpp ps2/kernel.cpp ps2/boot.cpp ps2/vif.cpp iop/sif_hle.cpp iop/services.cpp iop/memcard.cpp)
target_link_libraries(lantern_ps2 PUBLIC lantern_iso lantern_gs)
target_link_libraries(lantern_ps2 PUBLIC lantern_recomp_runtime lantern_analysis PRIVATE lantern_warnings)
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
foreach(shader placeholder.vert placeholder.frag image.frag)
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

add_executable(lantern-boot ps2/boot_main.cpp)
set_target_properties(lantern-boot PROPERTIES ENABLE_EXPORTS ON)  # the dlopen'ed game library links against the runtime
target_link_libraries(lantern-boot PRIVATE lantern_ps2 lantern_iso lantern_platform lantern_config ${CMAKE_DL_LIBS}
                      lantern_warnings)

# --- app ---
add_library(lantern_cli STATIC app/cli.cpp)
target_link_libraries(lantern_cli PUBLIC lantern_config PRIVATE lantern_warnings)

add_executable(lantern app/main.cpp)
target_compile_definitions(lantern PRIVATE LANTERN_VERSION="${PROJECT_VERSION}")
target_link_libraries(lantern PRIVATE lantern_cli lantern_platform lantern_render lantern_config SDL3::SDL3
                      lantern_warnings)
