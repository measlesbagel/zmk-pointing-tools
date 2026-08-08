if(CONFIG_ZMK_POINTING_TOOLS_TRACE OR
   CONFIG_ZMK_POINTING_TOOLS_TELEMETRY OR
   CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_CODEC OR
   CONFIG_ZMK_POINTING_TOOLS_NOISE_FILTER OR
   CONFIG_ZMK_POINTING_TOOLS_PIPELINE OR
   CONFIG_ZMK_POINTING_TOOLS_SCROLL OR
   CONFIG_ZMK_POINTING_TOOLS_TEXT_NAV OR
   CONFIG_ZMK_POINTING_TOOLS_SIGN_EXTEND_XY)
  zephyr_library()
  zephyr_include_directories(include)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_syscall_include_directories(include)

  # Composable pipeline architecture.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/core/pipeline.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/core/router.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/source/frame_assembler.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/source/motion_source.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_CODEC
    src/source/transport/compact_split_codec.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/stage/orientation.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/stage/pointer_identity.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/resolution_normalize.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_MOTION_GATE_CORE
    src/stage/motion_gate/coherent_displacement.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/motion_gate/coherent_displacement_stage.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/sink/cursor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink/cursor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/pipeline_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/router_executor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/router.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/orientation.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/pointer_identity.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/resolution_normalize.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/coherent_displacement.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/source_input_processor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_ENCODER
    src/platform/zmk/compact_split_encoder.c)

  # Monolithic processors retained during pipeline migration.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TRACE
    src/legacy_processor/zmk/input_processor_trace.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_NOISE_FILTER
    src/legacy_processor/noise_filter.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_NOISE_FILTER
    src/legacy_processor/zmk/input_processor_noise_filter.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_SCROLL
    src/legacy_processor/axis_intent.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_SCROLL src/legacy_processor/scroll.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_SCROLL
    src/legacy_processor/zmk/input_processor_scroll.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TEXT_NAV
    src/legacy_processor/zmk/input_processor_text_nav.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TEXT_NAV
    src/legacy_processor/text_nav.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_SIGN_EXTEND_XY
    src/legacy_processor/zmk/input_processor_sign_extend_xy.c)

  # Cross-cutting runtime services.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY src/service/telemetry.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING src/service/tuning.c)
endif()
