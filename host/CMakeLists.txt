cmake_minimum_required(VERSION 3.20)

project(zmk_pointing_tools_host LANGUAGES C)

include(CTest)

set(ZPT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(zpt_models STATIC
  "${ZPT_ROOT}/src/core/pipeline.c"
  "${ZPT_ROOT}/src/source/frame_assembler.c"
  "${ZPT_ROOT}/src/stage/pointer_identity.c"
  "${ZPT_ROOT}/src/legacy_processor/axis_intent.c"
  "${ZPT_ROOT}/src/legacy_processor/noise_filter.c"
  "${ZPT_ROOT}/src/legacy_processor/scroll.c"
  "${ZPT_ROOT}/src/legacy_processor/text_nav.c"
)
target_compile_features(zpt_models PUBLIC c_std_11)
target_include_directories(zpt_models PUBLIC "${ZPT_ROOT}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(zpt_models PRIVATE -Wall -Wextra -Werror)
endif()

function(zpt_host_executable target source)
  add_executable("${target}" "${source}")
  target_link_libraries("${target}" PRIVATE zpt_models)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options("${target}" PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

zpt_host_executable(axis_intent_test tests/axis_intent_test.c)
zpt_host_executable(frame_assembler_test tests/frame_assembler_test.c)
zpt_host_executable(noise_filter_test tests/noise_filter_test.c)
zpt_host_executable(pipeline_test tests/pipeline_test.c)
zpt_host_executable(text_nav_test tests/text_nav_test.c)
zpt_host_executable(zpt_noise_filter_replay runners/noise_filter_replay.c)
zpt_host_executable(zpt_scroll_replay runners/scroll_replay.c)
zpt_host_executable(zpt_text_nav_replay runners/text_nav_replay.c)

add_test(NAME axis_intent COMMAND axis_intent_test)
add_test(NAME frame_assembler COMMAND frame_assembler_test)
add_test(NAME noise_filter COMMAND noise_filter_test)
add_test(NAME motion_pipeline COMMAND pipeline_test)
add_test(NAME text_navigation COMMAND text_nav_test)

find_program(NODE_EXECUTABLE node REQUIRED)
file(GLOB ZPT_TRACE_FIXTURES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.json"
)
add_test(
  NAME trace_replay
  COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay/cli.js"
    --noise-runner "$<TARGET_FILE:zpt_noise_filter_replay>"
    --scroll-runner "$<TARGET_FILE:zpt_scroll_replay>"
    --text-runner "$<TARGET_FILE:zpt_text_nav_replay>"
    ${ZPT_TRACE_FIXTURES}
)
