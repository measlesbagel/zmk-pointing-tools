/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/pointing_tools/legacy_processor/scroll.h>

static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

static void emit_flush(struct zpt_scroll_state *state, const struct zpt_scroll_settings *settings,
                       uint32_t timestamp) {
    int16_t horizontal;
    int16_t vertical;
    if (zpt_scroll_flush(state, settings, &horizontal, &vertical, NULL)) {
        printf("O\t%" PRIu32 "\t%d\t%d\n", timestamp, horizontal, vertical);
    }
}

int main(void) {
    struct zpt_scroll_settings settings = {0};
    struct zpt_scroll_state state = {0};
    unsigned int discard;
    unsigned int policy_value;
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16
                     " %u %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %u",
               &settings.scale_multiplier, &settings.scale_divisor,
               &settings.report_interval_ms, &settings.idle_timeout_ms,
               &settings.suppress_after_keypress_ms, &discard,
               &settings.intent.engage_ratio_percent, &settings.intent.release_ratio_percent,
               &settings.intent.activation_distance, &settings.intent.window_ms,
               &policy_value) != 11 ||
        settings.scale_multiplier == 0 || settings.scale_divisor == 0 ||
        settings.report_interval_ms == 0 || policy_value > ZPT_AXIS_POLICY_VERTICAL) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }

    settings.discard_unclassified = discard != 0;
    enum zpt_axis_policy policy = (enum zpt_axis_policy)policy_value;
    state.policy = policy;
    zpt_scroll_reset(&state, true);

    bool flush_armed = false;
    uint32_t flush_at = 0;
    bool keypress_seen = false;
    uint32_t last_keypress = 0;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        int fields = sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp,
                            &x, &y);
        if (fields < 2) {
            fputs("invalid replay event\n", stderr);
            return 2;
        }

        if (flush_armed && due(timestamp, flush_at)) {
            emit_flush(&state, &settings, flush_at);
            flush_armed = false;
        }

        if (type == 'K') {
            keypress_seen = true;
            last_keypress = timestamp;
            continue;
        }
        if (type != 'M' || fields != 4) {
            fputs("unknown replay event\n", stderr);
            return 2;
        }

        bool suppress = settings.suppress_after_keypress_ms > 0 && keypress_seen &&
                        timestamp - last_keypress < settings.suppress_after_keypress_ms;
        struct zpt_scroll_decision decision =
            zpt_scroll_process(&state, &settings, x, y, policy, timestamp, suppress);
        printf("D\t%" PRIu32 "\t%d\t%" PRIu32 "\t%" PRIu32
               "\t%" PRId32 "\t%" PRId32 "\t%" PRId32 "\t%" PRId32
               "\t%" PRId32 "\t%" PRId32 "\t%d\t%d\n",
               timestamp, decision.intent, state.intent.horizontal_energy,
               state.intent.vertical_energy, state.undecided_x, state.undecided_y,
               state.pending_x, state.pending_y, state.remainder_x, state.remainder_y,
               decision.reset_for_idle, decision.suppressed);

        if (!decision.suppressed && !flush_armed) {
            flush_at = timestamp + settings.report_interval_ms;
            flush_armed = true;
        }
    }

    if (flush_armed) {
        emit_flush(&state, &settings, flush_at);
    }
    return 0;
}
