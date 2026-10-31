if(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY OR
   CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_CODEC OR
   CONFIG_ZMK_POINTING_TOOLS_PIPELINE OR
   CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
  zephyr_library()
  zephyr_include_directories(include)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_syscall_include_directories(include)

  # Composable pipeline architecture.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/core/pipeline.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/core/router.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/core/route_override.c)
  # The peripheral's compact split encoder assembles frames too.
  if(CONFIG_ZMK_POINTING_TOOLS_PIPELINE OR CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_ENCODER)
    zephyr_library_sources(src/source/frame_assembler.c)
  endif()
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/source/motion_source.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/source/source_ingress.c)
  # Physical pointing device discovery (both split halves).
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE src/source/device_caps.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE
    src/platform/zmk/device_table.c)
  # Split-routed sensor control: the agent serves requests on peripherals,
  # and the proxy relays them from the central.
  if(CONFIG_ZMK_POINTING_TOOLS_SENSOR_TUNNEL)
    if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
      zephyr_library_sources(src/source/sensor_control_session.c)
      zephyr_library_sources(src/platform/zmk/sensor_control_proxy.c)
    else()
      zephyr_library_sources(src/platform/zmk/sensor_control_agent.c)
    endif()
  endif()
  # Native sensor facet adapters (require their driver modules present).
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_SENSOR_CONTROL_PAW32XX
    src/platform/zmk/sensor_control_paw32xx.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_CODEC
    src/source/transport/compact_split_codec.c)
  # Core stage implementations (host-replayable algorithms).
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE src/stage/orientation.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/resolution_normalize.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/axis_intent/axis_intent.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/axis_intent/axis_intent_stage.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/axis_constraint/axis_constraint.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/scroll_batcher/scroll_batcher.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/text_nav/text_nav.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/cursor_transfer/cursor_transfer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/cursor_quantizer/cursor_quantizer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/cursor_acceleration/cursor_acceleration.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/cursor_coalescer/cursor_coalescer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/motion_gate/coherent_displacement.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/stage/motion_gate/coherent_displacement_stage.c)
  # Devicetree providers for stages, sinks, pipelines, and the router.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink/cursor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink/scroll.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/sink/action.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/keypress_suppression.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/pipeline_telemetry.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/pipeline_provider.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/router_executor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/router.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_ROUTE_BEHAVIOR
    src/platform/zmk/behavior_route.c)
  # Devicetree stage providers mirroring the core stage implementations.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/orientation.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/resolution_normalize.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/axis_intent.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/axis_constraint.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/scroll_batcher.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/text_nav.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/cursor_transfer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/cursor_quantizer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/cursor_acceleration.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/cursor_coalescer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/stage/coherent_displacement.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_PIPELINE
    src/platform/zmk/source_input_processor.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_COMPACT_SPLIT_ENCODER
    src/platform/zmk/compact_split_encoder.c)

  # Cross-cutting runtime services.
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY src/service/telemetry.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY
    src/service/transport/uart.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE
    src/service/transport/gatt.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING src/service/tuning.c)
  # Runtime stage tuning needs both the pipeline and the tuning service.
  if(CONFIG_ZMK_POINTING_TOOLS_PIPELINE AND CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    zephyr_library_sources(src/stage/stage_tuning.c)
    zephyr_library_sources(src/platform/zmk/stage_tuning.c)
  endif()
endif()
