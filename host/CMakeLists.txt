cmake_minimum_required(VERSION 3.20)

project(zmk_pointing_tools_host LANGUAGES C)

include(CTest)

set(ZPT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

option(ZPT_ENABLE_SANITIZERS
  "Build host targets with AddressSanitizer, UndefinedBehaviorSanitizer, and
  uninitialized auto-variable detection" OFF)

if(ZPT_ENABLE_SANITIZERS)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    # Fill trivially-initializable automatic variables with 0xAA... so a read
    # of an uninitialized variable fails the tests on a distinctive value
    # instead of passing on stack garbage (ASan/UBSan do not catch that
    # class). Requires GCC 12+ or Clang 15+ (both true of the devenv tools).
    add_compile_options(-ftrivial-auto-var-init=pattern)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(WARNING "ZPT_ENABLE_SANITIZERS requires GCC or Clang; ignoring")
  endif()
endif()

add_library(zpt_models STATIC
  "${ZPT_ROOT}/src/core/pipeline.c"
  "${ZPT_ROOT}/src/core/router.c"
  "${ZPT_ROOT}/src/core/route_override.c"
  "${ZPT_ROOT}/src/source/frame_assembler.c"
  "${ZPT_ROOT}/src/source/motion_source.c"
  "${ZPT_ROOT}/src/source/transport/compact_split_codec.c"
  "${ZPT_ROOT}/src/stage/orientation.c"
  "${ZPT_ROOT}/src/stage/resolution_normalize.c"
  "${ZPT_ROOT}/src/stage/axis_intent/axis_intent.c"
  "${ZPT_ROOT}/src/stage/axis_intent/axis_intent_stage.c"
  "${ZPT_ROOT}/src/stage/axis_constraint/axis_constraint.c"
  "${ZPT_ROOT}/src/stage/scroll_batcher/scroll_batcher.c"
  "${ZPT_ROOT}/src/stage/text_nav/text_nav.c"
  "${ZPT_ROOT}/src/stage/cursor_transfer/cursor_transfer.c"
  "${ZPT_ROOT}/src/stage/cursor_quantizer/cursor_quantizer.c"
  "${ZPT_ROOT}/src/stage/motion_gate/coherent_displacement.c"
  "${ZPT_ROOT}/src/stage/motion_gate/coherent_displacement_stage.c"
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

zpt_host_executable(axis_intent_stage_test tests/axis_intent_stage_test.c)
zpt_host_executable(compact_split_codec_test tests/compact_split_codec_test.c)
zpt_host_executable(frame_assembler_test tests/frame_assembler_test.c)
zpt_host_executable(motion_gate_test tests/motion_gate_test.c)
zpt_host_executable(pipeline_test tests/pipeline_test.c)
zpt_host_executable(router_test tests/router_test.c)
zpt_host_executable(route_override_test tests/route_override_test.c)
zpt_host_executable(pipeline_scroll_text_test tests/pipeline_scroll_text_test.c)
zpt_host_executable(stage_observer_test tests/stage_observer_test.c)
zpt_host_executable(cursor_pipeline_test tests/cursor_pipeline_test.c)
zpt_host_executable(source_normalization_test tests/source_normalization_test.c)
zpt_host_executable(zpt_noise_pipeline_replay runners/noise_pipeline_replay.c)
zpt_host_executable(zpt_scroll_pipeline_replay runners/scroll_pipeline_replay.c)
zpt_host_executable(zpt_cursor_pipeline_replay runners/cursor_pipeline_replay.c)
zpt_host_executable(zpt_text_nav_pipeline_replay runners/text_nav_pipeline_replay.c)

add_test(NAME axis_intent_stage COMMAND axis_intent_stage_test)
add_test(NAME compact_split_codec COMMAND compact_split_codec_test)
add_test(NAME frame_assembler COMMAND frame_assembler_test)
add_test(NAME motion_gate COMMAND motion_gate_test)
add_test(NAME motion_pipeline COMMAND pipeline_test)
add_test(NAME pipeline_router COMMAND router_test)
add_test(NAME route_override COMMAND route_override_test)
add_test(NAME pipeline_scroll_text COMMAND pipeline_scroll_text_test)
add_test(NAME stage_observer COMMAND stage_observer_test)
add_test(NAME cursor_pipeline COMMAND cursor_pipeline_test)
add_test(NAME source_normalization COMMAND source_normalization_test)

find_program(NODE_EXECUTABLE node REQUIRED)
file(GLOB ZPT_TRACE_FIXTURES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.json"
)
add_test(
  NAME trace_replay
  COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay/cli.js"
    --noise-pipeline-runner "$<TARGET_FILE:zpt_noise_pipeline_replay>"
    --scroll-pipeline-runner "$<TARGET_FILE:zpt_scroll_pipeline_replay>"
    --cursor-pipeline-runner "$<TARGET_FILE:zpt_cursor_pipeline_replay>"
    --text-pipeline-runner "$<TARGET_FILE:zpt_text_nav_pipeline_replay>"
    ${ZPT_TRACE_FIXTURES}
)
