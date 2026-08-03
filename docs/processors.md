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
        tuning-id = "left-scroll";
        tuning-label = "Left scroll";
        scale-multiplier = <1>;
        scale-divisor = <8>;
        report-interval-ms = <16>;
        idle-timeout-ms = <120>;
        intent-window-ms = <64>;
        activation-distance = <16>;
        engage-ratio-percent = <300>;
        release-ratio-percent = <180>;
        /* Optional for an always-on dedicated scroll device. */
        discard-unclassified;
        suppress-after-keypress-ms = <75>;
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

`discard-unclassified` turns `activation-distance` into a motion gate: a
gesture that ends before classification produces no scroll output. This is
useful for an always-on dedicated scroll device where typing vibration should
not move the document. Leave it unset for a momentary Scroll layer where very
short intentional gestures should be preserved.

`suppress-after-keypress-ms` uses actual matrix key presses instead of motion
magnitude to reject typing vibration. For a dedicated scroll device, this can
be used without `discard-unclassified` so fine movement remains available when
the keyboard is idle. A held modifier only suppresses the beginning of a
gesture; scrolling resumes when the configured interval has elapsed.

When the USB telemetry service is enabled, every scroll instance is also
available as a temporary runtime tuning target. `tuning-label` gives it a
human-readable identity. Scale, timing, axis-intent, keypress-guard, and
unclassified-motion settings can be previewed in RAM and reset to these
compiled devicetree values. Runtime previews never write flash.

`tuning-id` is a stable machine-readable identity used by exported profiles.
Keep it unique within a firmware build and avoid changing it unless profiles
should intentionally stop matching that target.

## Text navigation

Text navigation chooses an axis from gesture geometry before applying the
independent horizontal and vertical step distances:

```dts
/ {
    my_text_nav: my_text_nav {
        compatible = "measlesbagel,zpt-input-processor-text-nav";
        #input-processor-cells = <0>;
        tuning-id = "text-navigation";
        tuning-label = "Text navigation";
        horizontal-threshold = <25>;
        vertical-threshold = <50>;
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
                           <&my_text_nav>;
    };
};
```

Once selected, the axis remains locked until `idle-timeout-ms` elapses. At
most one behavior tap is emitted per physical input frame, preventing a fast
sensor report from enqueueing a large burst of arrows at once.

When runtime tuning is enabled, each text-navigation instance exposes its
horizontal and vertical thresholds, activation distance, engage ratio, and
idle timeout. A preview resets the active gesture before applying the new
value. `tuning-label` identifies the instance without imposing left/right
device semantics.

## Signed packed split axes

Some packed X/Y processors transport each axis as a 16-bit field but unpack it
without sign extension. Place a sign-extension processor immediately after the
unpacker so negative movement does not appear as values near 65535:

```dts
my_sign_extend: my_sign_extend {
    compatible = "measlesbagel,zpt-input-processor-sign-extend-xy";
    #input-processor-cells = <0>;
};

&my_central_split_listener {
    input-processors = <&my_xy_unpack>, <&my_sign_extend>, <&my_semantic_processor>;
};
```
