/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include <zmk/pointing_tools/legacy_processor/noise_filter.h>

int main(void) {
    struct zpt_noise_filter_settings settings = {0};
    struct zpt_noise_filter_state state = {0};
    unsigned int enabled;
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %u %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16, &enabled,
               &settings.activation_distance, &settings.coherence_percent,
               &settings.qualification_timeout_ms, &settings.idle_timeout_ms,
               &settings.suppress_after_keypress_ms) != 6) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }
    settings.enabled = enabled != 0;
    zpt_noise_filter_reset(&state);

    bool keypress_seen = false;
    uint32_t last_keypress = 0;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        int fields =
            sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp, &x, &y);
        if (fields < 2) {
            fputs("invalid replay event\n", stderr);
            return 2;
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
        struct zpt_noise_filter_result result =
            zpt_noise_filter_update(&state, &settings, x, y, timestamp, suppress);
        printf("D\t%" PRIu32 "\t%d\t%" PRId32 "\t%" PRId32 "\t%" PRIu32 "\t%" PRIu64
               "\t%d\t%d\t%d\t%d\t%d\t%" PRId32 "\t%" PRId32 "\n",
               timestamp, result.phase, state.pending_x, state.pending_y, state.sample_count,
               state.squared_energy, result.reset_for_idle, result.reset_for_timeout,
               result.suppressed, result.qualified, result.discarded, result.x, result.y);
        if (result.x != 0 || result.y != 0) {
            printf("O\t%" PRIu32 "\t%" PRId32 "\t%" PRId32 "\n", timestamp, result.x, result.y);
        }
    }
    return 0;
}
