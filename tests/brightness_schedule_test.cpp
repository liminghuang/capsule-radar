#include "brightness_schedule.h"

#include <assert.h>

static BrightnessScheduleRule rule(uint8_t days, int start, int end, int percent) {
    BrightnessScheduleRule r{};
    r.enabled = true;
    r.dayMask = days;
    r.startMinute = (uint16_t)start;
    r.endMinute = (uint16_t)end;
    r.brightnessPercent = (uint8_t)percent;
    return r;
}

int main() {
    constexpr uint8_t MON_TO_FRI = 0x1F;
    constexpr uint8_t EVERY_DAY = 0x7F;
    int percent = -1;

    // tm_wday-compatible input: Sunday=0, Monday=1, ... Saturday=6.
    BrightnessScheduleRule office[] = {rule(MON_TO_FRI, 8 * 60, 17 * 60, 80)};
    assert(brightnessSchedulePercent(office, 1, 1, 8, 0, percent) && percent == 80);
    assert(brightnessSchedulePercent(office, 1, 5, 16, 59, percent) && percent == 80);
    assert(!brightnessSchedulePercent(office, 1, 5, 17, 0, percent));
    assert(!brightnessSchedulePercent(office, 1, 6, 10, 0, percent));

    // A cross-midnight Monday rule owns both Monday night and Tuesday morning.
    BrightnessScheduleRule night[] = {rule(1u << 0, 20 * 60, 8 * 60, 10)};
    assert(brightnessSchedulePercent(night, 1, 1, 20, 0, percent) && percent == 10);
    assert(brightnessSchedulePercent(night, 1, 2, 7, 59, percent) && percent == 10);
    assert(!brightnessSchedulePercent(night, 1, 2, 8, 0, percent));
    assert(!brightnessSchedulePercent(night, 1, 2, 21, 0, percent));

    // Later matching rules take priority, allowing a specific override.
    BrightnessScheduleRule layered[] = {
        rule(EVERY_DAY, 20 * 60, 8 * 60, 10),
        rule(1u << 4, 22 * 60, 23 * 60, 25),
    };
    assert(brightnessSchedulePercent(layered, 2, 5, 22, 30, percent) && percent == 25);
    assert(brightnessSchedulePercent(layered, 2, 6, 1, 0, percent) && percent == 10);

    BrightnessScheduleRule disabled = rule(EVERY_DAY, 0, 0, 50);
    disabled.enabled = false;
    assert(!brightnessSchedulePercent(&disabled, 1, 1, 12, 0, percent));

    assert(brightnessPercentToPanel(1) == 3);
    assert(brightnessPercentToPanel(10) == 26);
    assert(brightnessPercentToPanel(80) == 204);
    assert(brightnessPercentToPanel(100) == 255);
}
