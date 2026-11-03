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
set_tests_properties(gpu_list_devices gpu_vulkan_frames gpu_vulkan_mailbox gpu_vulkan_swapchain_recreate PROPERTIES
  LABELS gpu RUN_SERIAL TRUE ENVIRONMENT "${SMOKE_ENV}")
