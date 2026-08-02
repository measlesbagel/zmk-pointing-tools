# Semantic processors

## Axis policies

Include the policy constants in a keymap:

```dts
#include <dt-bindings/zmk/pointing_tools.h>
```

The scroll processor accepts one input-processor parameter:

- `ZPT_AXIS_FREE` preserves both axes.
- `ZPT_AXIS_ADAPTIVE` dynamically suppresses an unintended minor axis.
- `ZPT_AXIS_HORIZONTAL` emits horizontal scrolling only.
- `ZPT_AXIS_VERTICAL` emits vertical scrolling only.

## Synchronized scroll

The scroll processor consumes physical X/Y events and emits coalesced wheel
events from its own device. Give that virtual source a dedicated ZMK input
listener:

```dts
/ {
    my_scroll: my_scroll {
        compatible = "measlesbagel,zpt-input-processor-scroll";
        #input-processor-cells = <1>;
        scale-multiplier = <1>;
        scale-divisor = <8>;
        report-interval-ms = <16>;
        idle-timeout-ms = <120>;
        intent-window-ms = <64>;
        activation-distance = <16>;
        engage-ratio-percent = <300>;
        release-ratio-percent = <180>;
    };

    my_scroll_output: my_scroll_output {
        compatible = "zmk,input-listener";
        device = <&my_scroll>;
    };
};

&my_physical_input_listener {
    input-processors = <&my_orientation_transform>,
                       <&my_scroll ZPT_AXIS_ADAPTIVE>;
};
```

The processor applies axis intent to complete frames, accumulates fractional
movement independently for both axes, and emits horizontal wheel followed by
vertical wheel as one synchronized report. Its delayable flush also preserves
short movements that end between physical sensor frames.

`engage-ratio-percent` should be greater than
`release-ratio-percent`. The gap provides hysteresis so noisy frames do not
rapidly enter and leave an axis lock.

## Text navigation

Text navigation chooses an axis from gesture geometry before applying the
independent horizontal and vertical step distances:

```dts
/ {
    my_text_nav: my_text_nav {
        compatible = "measlesbagel,zpt-input-processor-text-nav";
        #input-processor-cells = <2>;
        bindings = <&kp LEFT>, <&kp RIGHT>, <&kp UP>, <&kp DOWN>;
        tap-ms = <5>;
        idle-timeout-ms = <120>;
        activation-distance = <12>;
        engage-ratio-percent = <150>;
    };
};

&my_physical_input_listener {
    text_mode {
        layers = <TEXT>;
        input-processors = <&my_orientation_transform>,
                           <&my_text_nav 25 50>;
    };
};
```

Once selected, the axis remains locked until `idle-timeout-ms` elapses. At
most one behavior tap is emitted per physical input frame, preventing a fast
sensor report from enqueueing a large burst of arrows at once.
