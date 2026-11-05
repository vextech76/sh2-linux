find_package(doctest REQUIRED CONFIG)

add_library(lantern_test_main STATIC test_main.cpp)
target_link_libraries(lantern_test_main PUBLIC doctest::doctest)

function(lantern_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE lantern_test_main lantern_warnings ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lantern_test(test_paths lantern_core)
lantern_test(test_pad lantern_input)
lantern_test(test_deadzone lantern_input)
lantern_test(test_bindings lantern_input)
lantern_test(test_input_system lantern_input)
lantern_test(test_config lantern_config)
lantern_test(test_cli lantern_cli)
lantern_test(test_null_renderer lantern_render)
lantern_test(test_iso lantern_iso)
lantern_test(test_elf lantern_ee)
lantern_test(test_r5900 lantern_ee)
lantern_test(test_analysis lantern_analysis)
lantern_test(test_interpreter lantern_ee)
lantern_test(test_semantics_ext lantern_ee)
lantern_test(test_kernel lantern_ps2)
lantern_test(test_sif lantern_ps2)
lantern_test(test_iop_services lantern_ps2)
lantern_test(test_r3000 lantern_iop)
lantern_test(test_irx lantern_iop)
lantern_test(test_hle lantern_iop)
lantern_test(test_gs lantern_ps2)

# Recompiler differential test: generate C++ from synthetic programs at build time, compile it,
# and compare against the interpreter. Generated code is built like real recompiled output:
# no project warnings, and FP contraction off so float results match the interpreter exactly.
add_executable(recomp_selftest_gen recomp_selftest_gen.cpp)
target_link_libraries(recomp_selftest_gen PRIVATE lantern_recomp)
set(SELFTEST_GEN ${CMAKE_CURRENT_BINARY_DIR}/recomp_selftest_generated.cpp)
add_custom_command(OUTPUT ${SELFTEST_GEN} COMMAND recomp_selftest_gen ${SELFTEST_GEN}
                   DEPENDS recomp_selftest_gen ${CMAKE_CURRENT_SOURCE_DIR}/recomp_programs.h
                   COMMENT "recompiling synthetic test programs")
add_library(recomp_selftest_code STATIC ${SELFTEST_GEN})
target_link_libraries(recomp_selftest_code PUBLIC lantern_recomp_runtime)
target_compile_options(recomp_selftest_code PRIVATE -ffp-contract=off -Wno-unused-label)
lantern_test(test_recomp lantern_recomp_runtime recomp_selftest_code)
# GCC 16 -O2 false positive in std::vector initializer-list copies (recomp_programs.h).
target_compile_options(test_recomp PRIVATE -ffp-contract=off -Wno-array-bounds)

# Optional differential test against GNU binutils (sudo dnf install binutils-mips64-linux-gnu).
find_program(MIPS_OBJDUMP mips64-linux-gnu-objdump)
find_package(Python3 COMPONENTS Interpreter)
if(MIPS_OBJDUMP AND Python3_FOUND)
  add_test(NAME oracle_r5900_sweep COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/r5900_oracle_diff.py
           $<TARGET_FILE:lantern-disasm>)
  add_test(NAME oracle_r5900_random COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/r5900_oracle_diff.py
           $<TARGET_FILE:lantern-disasm> --random 300000)
  set_tests_properties(oracle_r5900_sweep oracle_r5900_random PROPERTIES LABELS oracle)
endif()
lantern_test(test_ingest lantern_ingest)
add_test(NAME smoke_ingest_usage COMMAND lantern-ingest)
set_tests_properties(smoke_ingest_usage PROPERTIES WILL_FAIL TRUE)

# Smoke tests of the real executable. Each run gets an isolated XDG home so nothing touches
# the developer's real settings.
set(SMOKE_ENV
  "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/config"
  "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/data"
  "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/cache"
  "ASAN_OPTIONS=detect_leaks=0")

add_test(NAME smoke_help COMMAND lantern --help)
add_test(NAME smoke_null_backend COMMAND lantern --backend null --frames 5)
add_test(NAME smoke_print_config COMMAND lantern --backend null --print-config)
add_test(NAME smoke_bad_option COMMAND lantern --bogus)
set_tests_properties(smoke_bad_option PROPERTIES WILL_FAIL TRUE)
set_tests_properties(smoke_help smoke_null_backend smoke_print_config smoke_bad_option PROPERTIES ENVIRONMENT "${SMOKE_ENV}")

# GPU tests: need a display and a Vulkan 1.3 GPU. Run with: ctest --preset debug-gpu
add_test(NAME gpu_list_devices COMMAND lantern --list-gpus)
add_test(NAME gpu_vulkan_frames COMMAND lantern --backend vulkan --validation auto --frames 240)
add_test(NAME gpu_vulkan_mailbox COMMAND lantern --backend vulkan --frames 120 --config ${CMAKE_CURRENT_SOURCE_DIR}/data/mailbox.toml)
add_test(NAME gpu_vulkan_swapchain_recreate COMMAND lantern --backend vulkan --validation on --frames 200 --exercise-swapchain)
add_test(NAME gpu_vulkan_image_upload COMMAND lantern --backend vulkan --validation on --frames 150 --exercise-image)
set_tests_properties(gpu_list_devices gpu_vulkan_frames gpu_vulkan_mailbox gpu_vulkan_swapchain_recreate gpu_vulkan_image_upload PROPERTIES
  LABELS gpu RUN_SERIAL TRUE ENVIRONMENT "${SMOKE_ENV}")
