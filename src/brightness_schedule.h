#pragma once

#include <stddef.h>
#include <stdint.h>

struct BrightnessScheduleRule {
    bool enabled = false;
    uint8_t dayMask = 0x7F;          // bit 0=Monday ... bit 6=Sunday
    uint16_t startMinute = 0;        // local minutes after midnight
    uint16_t endMinute = 0;          // exclusive; equal start/end means all day
    uint8_t brightnessPercent = 50;  // 1..100
};

inline int brightnessDayIndex(int tmWday) {
    return (tmWday + 6) % 7;  // struct tm: Sunday=0 -> schedule: Sunday=6
}

inline bool brightnessRuleMatches(const BrightnessScheduleRule& rule,
                                  int tmWday, int hour, int minute) {
    if (!rule.enabled || rule.dayMask == 0 || tmWday < 0 || tmWday > 6 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        rule.startMinute >= 24 * 60 || rule.endMinute >= 24 * 60)
        return false;

    const int today = brightnessDayIndex(tmWday);
    const int nowMinute = hour * 60 + minute;
    const auto selected = [&](int day) { return (rule.dayMask & (1u << day)) != 0; };

    if (rule.startMinute == rule.endMinute)
        return selected(today);  // explicit all-day rule
    if (rule.startMinute < rule.endMinute)
        return selected(today) && nowMinute >= rule.startMinute && nowMinute < rule.endMinute;

    // Cross-midnight: early-morning time belongs to the previous selected day.
    if (nowMinute >= rule.startMinute) return selected(today);
    if (nowMinute < rule.endMinute) return selected((today + 6) % 7);
    return false;
}

inline bool brightnessSchedulePercent(const BrightnessScheduleRule* rules, size_t count,
                                      int tmWday, int hour, int minute, int& percentOut) {
    bool matched = false;
    for (size_t i = 0; i < count; ++i) {
        if (!brightnessRuleMatches(rules[i], tmWday, hour, minute)) continue;
        int percent = rules[i].brightnessPercent;
        if (percent < 1) percent = 1;
        if (percent > 100) percent = 100;
        percentOut = percent;  // later matching rules take priority
        matched = true;
    }
    return matched;
}

inline int brightnessPercentToPanel(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    return (percent * 255 + 50) / 100;
}
