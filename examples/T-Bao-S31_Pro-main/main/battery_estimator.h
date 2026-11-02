/* Approximate state-of-charge estimation for the T-Bao S31 2S battery pack. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert the measured pack voltage to an approximate percentage.
 *
 * A negative return value means that the voltage is not usable. The result is
 * deliberately an estimate, not a coulomb-counter reading; the table should
 * be calibrated against the production battery pack after field measurements.
 */
int battery_estimator_percent_from_mv(int voltage_mv);

/**
 * Limit a single-sample percentage jump to five points.
 *
 * A previous value below zero means that no history is available. A measured
 * value below zero invalidates the result instead of retaining stale data.
 */
int battery_estimator_limit_step(int previous_percent, int measured_percent);

#ifdef __cplusplus
}
#endif
