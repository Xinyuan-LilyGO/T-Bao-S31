#include "battery_estimator.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int voltage_mv;
    int percent;
} battery_voltage_point_t;

/* Open-circuit approximation for a typical 2S Li-ion pack. The end points
 * intentionally leave room for ADC noise and are expected to be tuned after
 * measuring the actual pack under the product's normal load. */
static const battery_voltage_point_t kVoltageTable[] = {
    {6000, 0},
    {6400, 10},
    {6600, 20},
    {6800, 30},
    {7000, 40},
    {7200, 50},
    {7400, 60},
    {7600, 70},
    {7800, 80},
    {8000, 90},
    {8200, 95},
    {8400, 100},
};

static int clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

int battery_estimator_percent_from_mv(int voltage_mv)
{
    const size_t point_count = sizeof(kVoltageTable) / sizeof(kVoltageTable[0]);
    if (voltage_mv <= 0) {
        return -1;
    }
    if (voltage_mv <= kVoltageTable[0].voltage_mv) {
        return kVoltageTable[0].percent;
    }
    if (voltage_mv >= kVoltageTable[point_count - 1].voltage_mv) {
        return kVoltageTable[point_count - 1].percent;
    }

    for (size_t i = 1; i < point_count; ++i) {
        const battery_voltage_point_t lower = kVoltageTable[i - 1];
        const battery_voltage_point_t upper = kVoltageTable[i];
        if (voltage_mv <= upper.voltage_mv) {
            const int voltage_span = upper.voltage_mv - lower.voltage_mv;
            const int percent_span = upper.percent - lower.percent;
            const int voltage_offset = voltage_mv - lower.voltage_mv;
            const int64_t scaled = (int64_t)voltage_offset * percent_span;
            return lower.percent + (int)((scaled + voltage_span / 2) / voltage_span);
        }
    }

    return 100;
}

int battery_estimator_limit_step(int previous_percent, int measured_percent)
{
    if (measured_percent < 0) {
        return -1;
    }

    measured_percent = clamp_percent(measured_percent);
    if (previous_percent < 0 || previous_percent > 100) {
        return measured_percent;
    }

    const int maximum_step = 5;
    if (measured_percent > previous_percent + maximum_step) {
        return previous_percent + maximum_step;
    }
    if (measured_percent < previous_percent - maximum_step) {
        return previous_percent - maximum_step;
    }
    return measured_percent;
}
