/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include <zmk/pointing_tools/text_nav.h>

int main(void) {
    struct zpt_text_nav_settings settings = {0};
    struct zpt_text_nav_state state = {0};
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16,
               &settings.horizontal_threshold, &settings.vertical_threshold,
               &settings.idle_timeout_ms, &settings.activation_distance,
               &settings.engage_ratio_percent) != 5 ||
        settings.horizontal_threshold == 0 || settings.vertical_threshold == 0 ||
        settings.idle_timeout_ms == 0) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }

    zpt_text_nav_reset(&state);
    bool have_last_frame = false;
    uint32_t last_frame = 0;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        if (sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp, &x, &y) !=
                4 ||
            type != 'M') {
            fputs("text navigation only accepts motion events\n", stderr);
            return 2;
        }

        uint32_t elapsed = have_last_frame ? timestamp - last_frame : 0;
        enum zpt_text_nav_direction direction = zpt_text_nav_update(
            &state, &settings, x, y, elapsed, have_last_frame);
        have_last_frame = true;
        last_frame = timestamp;
        printf("D\t%" PRIu32 "\t%d\t%" PRId32 "\t%" PRId32 "\t%d\n", timestamp,
               state.intent, state.accumulated_x, state.accumulated_y, direction);
        if (direction != ZPT_TEXT_NAV_NONE) {
            printf("O\t%" PRIu32 "\t%d\n", timestamp, direction);
        }
    }
    return 0;
}
