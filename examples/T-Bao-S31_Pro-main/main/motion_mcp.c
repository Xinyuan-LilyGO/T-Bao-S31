#include "motion_mcp.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "actuator_control.h"
#include "app_face_follow.h"
#include "esp_log.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "motion_behavior.h"
#include "motion_manager.h"

#define MOTION_MCP_MIN_DURATION_MS 100
#define MOTION_MCP_MAX_DURATION_MS 10000
#define MOTION_MCP_SERVO_HOLD_MS 10000U

static const char *TAG = "MOTION_MCP";

static esp_mcp_value_t result_from_error(esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "MCP motion request rejected: %s", esp_err_to_name(error));
    }
    return esp_mcp_value_create_bool(error == ESP_OK);
}

static esp_mcp_value_t motion_drive_callback(const esp_mcp_property_list_t *properties)
{
    const int linear_percent =
        esp_mcp_property_list_get_property_int(properties, "linear_percent");
    const int angular_percent =
        esp_mcp_property_list_get_property_int(properties, "angular_percent");
    const int duration_ms =
        esp_mcp_property_list_get_property_int(properties, "duration_ms");

    if (linear_percent < -100 || linear_percent > 100 ||
        angular_percent < -100 || angular_percent > 100 ||
        duration_ms < MOTION_MCP_MIN_DURATION_MS ||
        duration_ms > MOTION_MCP_MAX_DURATION_MS) {
        ESP_LOGW(TAG, "Invalid drive request: linear=%d angular=%d duration=%d",
                 linear_percent, angular_percent, duration_ms);
        return esp_mcp_value_create_bool(false);
    }

    return result_from_error(motion_behavior_submit_mcp(
        (float)linear_percent / 100.0f,
        (float)angular_percent / 100.0f,
        (uint32_t)duration_ms));
}

static esp_mcp_value_t motion_spin_callback(const esp_mcp_property_list_t *properties)
{
    const bool clockwise =
        esp_mcp_property_list_get_property_bool(properties, "clockwise");
    const int speed_percent =
        esp_mcp_property_list_get_property_int(properties, "speed_percent");
    const int duration_ms =
        esp_mcp_property_list_get_property_int(properties, "duration_ms");

    if (speed_percent < 18 || speed_percent > 100 ||
        duration_ms < MOTION_MCP_MIN_DURATION_MS ||
        duration_ms > MOTION_MCP_MAX_DURATION_MS) {
        ESP_LOGW(TAG, "Invalid spin request: clockwise=%d speed=%d duration=%d",
                 clockwise, speed_percent, duration_ms);
        return esp_mcp_value_create_bool(false);
    }

    /* Existing vehicle calibration uses positive angular input for a left
     * turn, so clockwise is represented by a negative angular request. Keep
     * linear at zero: both controllable wheels must run at equal magnitude
     * and opposite signs for an in-place pivot around the chassis center. */
    const float angular = (float)speed_percent / 100.0f *
                          (clockwise ? -1.0f : 1.0f);
    ESP_LOGI(TAG, "MCP spin: clockwise=%d speed=%d duration=%d -> linear=0 angular=%.2f",
             clockwise, speed_percent, duration_ms, angular);
    return result_from_error(motion_behavior_submit_mcp(
        0.0f, angular, (uint32_t)duration_ms));
}

static esp_mcp_value_t motion_spin_360_callback(
    const esp_mcp_property_list_t *properties)
{
    const bool clockwise =
        esp_mcp_property_list_get_property_bool(properties, "clockwise");
    ESP_LOGI(TAG, "MCP spin_360: clockwise=%d", clockwise ? 1 : 0);
    return result_from_error(motion_behavior_start_spin_360(clockwise));
}

static esp_mcp_value_t motion_stop_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    (void)app_face_follow_stop();
    motion_manager_stop_all();
    return esp_mcp_value_create_bool(true);
}

static const char *source_name(motion_source_t source)
{
    switch (source) {
    case MOTION_SOURCE_EXPRESSION:
        return "expression";
    case MOTION_SOURCE_VOICE:
        return "voice";
    case MOTION_SOURCE_MCP:
        return "mcp";
    case MOTION_SOURCE_TOUCH:
        return "touch";
    case MOTION_SOURCE_WEB:
        return "web";
    case MOTION_SOURCE_SAFETY:
        return "safety";
    case MOTION_SOURCE_FOLLOW:
        return "follow";
    default:
        return "none";
    }
}

static esp_mcp_value_t motion_status_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    motion_state_t state;
    motion_manager_get_state(&state);

    char response[768];
    const float heading_deg = state.pose.theta_rad * 180.0f / 3.14159265358979323846f;
    const int written = snprintf(
        response, sizeof(response),
        "{\"available\":%s,\"faulted\":%s,\"failsafe\":%s,"
        "\"activeSource\":\"%s\",\"sequenceActive\":%s,"
        "\"requestedLinear\":%.3f,\"requestedAngular\":%.3f,"
        "\"motorA\":%d,\"motorB\":%d,"
        "\"targetMotorA\":%d,\"targetMotorB\":%d,"
        "\"servoUs\":%d,\"servoEnabled\":%s,"
        "\"poseX\":%.2f,\"poseY\":%.2f,\"headingDeg\":%.2f,"
        "\"sequenceIndex\":%u,\"sequenceCount\":%u}",
        state.available ? "true" : "false",
        state.faulted ? "true" : "false",
        state.failsafe_active ? "true" : "false",
        source_name(state.active_source),
        state.sequence_active ? "true" : "false",
        state.requested_twist.linear,
        state.requested_twist.angular,
        state.current_motor_a_percent,
        state.current_motor_b_percent,
        state.target_motor_a_percent,
        state.target_motor_b_percent,
        state.current_servo_pulse_us,
        state.servo_enabled ? "true" : "false",
        state.pose.x_cm,
        state.pose.y_cm,
        heading_deg,
        (unsigned)state.sequence_index,
        (unsigned)state.sequence_count);

    if (written < 0 || (size_t)written >= sizeof(response)) {
        ESP_LOGE(TAG, "Failed to format motion status");
        return esp_mcp_value_create_bool(false);
    }
    return esp_mcp_value_create_string(response);
}

static int servo_angle_to_pulse(int angle_deg)
{
    return ACTUATOR_SERVO_PULSE_CENTER_US +
           (angle_deg * (ACTUATOR_SERVO_PULSE_MAX_US -
                         ACTUATOR_SERVO_PULSE_CENTER_US)) / 90;
}

static esp_mcp_value_t servo_set_angle_callback(const esp_mcp_property_list_t *properties)
{
    const int angle_deg =
        esp_mcp_property_list_get_property_int(properties, "angle_deg");
    if (angle_deg < -90 || angle_deg > 90) {
        ESP_LOGW(TAG, "Invalid servo angle: %d", angle_deg);
        return esp_mcp_value_create_bool(false);
    }

    return result_from_error(motion_behavior_submit_mcp_servo(
        servo_angle_to_pulse(angle_deg), MOTION_MCP_SERVO_HOLD_MS));
}

static esp_mcp_value_t servo_nod_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    return result_from_error(
        motion_behavior_start_named(MOTION_BEHAVIOR_ACTION_NOD));
}

static esp_mcp_value_t servo_center_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    return result_from_error(motion_behavior_submit_mcp_servo(
        ACTUATOR_SERVO_PULSE_CENTER_US, MOTION_MCP_SERVO_HOLD_MS));
}

static esp_err_t add_tool(esp_mcp_t *mcp,
                          const char *name,
                          const char *description,
                          esp_mcp_tool_callback_t callback,
                          esp_mcp_property_t **properties,
                          size_t property_count)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(name, description, callback);
    if (tool == NULL) {
        for (size_t i = 0; i < property_count; ++i) {
            if (properties[i] != NULL) {
                esp_mcp_property_destroy(properties[i]);
            }
        }
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < property_count; ++i) {
        if (properties[i] == NULL) {
            for (size_t j = i + 1; j < property_count; ++j) {
                if (properties[j] != NULL) {
                    esp_mcp_property_destroy(properties[j]);
                }
            }
            esp_mcp_tool_destroy(tool);
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t property_ret =
            esp_mcp_tool_add_property(tool, properties[i]);
        if (property_ret != ESP_OK) {
            esp_mcp_property_destroy(properties[i]);
            for (size_t j = i + 1; j < property_count; ++j) {
                if (properties[j] != NULL) {
                    esp_mcp_property_destroy(properties[j]);
                }
            }
            esp_mcp_tool_destroy(tool);
            return property_ret;
        }
    }

    const esp_err_t add_ret = esp_mcp_add_tool(mcp, tool);
    if (add_ret != ESP_OK) {
        esp_mcp_tool_destroy(tool);
    }
    return add_ret;
}

static esp_err_t add_required_int_tool(esp_mcp_t *mcp,
                                       const char *name,
                                       const char *description,
                                       esp_mcp_tool_callback_t callback,
                                       const char *property_name,
                                       int minimum,
                                       int maximum)
{
    esp_mcp_property_t *property =
        esp_mcp_property_create_with_range(property_name, minimum, maximum);
    esp_mcp_property_t *properties[] = {property};
    return add_tool(mcp, name, description, callback, properties, 1);
}

esp_err_t motion_mcp_register_tools(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_mcp_property_t *drive_properties[] = {
        esp_mcp_property_create_with_range("linear_percent", -100, 100),
        esp_mcp_property_create_with_range("angular_percent", -100, 100),
        esp_mcp_property_create_with_range("duration_ms",
                                           MOTION_MCP_MIN_DURATION_MS,
                                           MOTION_MCP_MAX_DURATION_MS),
    };
    esp_err_t ret = add_tool(
        mcp,
        "self.motion.drive",
        "Drive the car with differential wheels. linear_percent: forward positive and reverse negative; angular_percent: left positive and right negative; provide all three values.",
        motion_drive_callback,
        drive_properties,
        sizeof(drive_properties) / sizeof(drive_properties[0]));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_mcp_property_t *spin_properties[] = {
        esp_mcp_property_create("clockwise", ESP_MCP_PROPERTY_TYPE_BOOLEAN),
        esp_mcp_property_create_with_range("speed_percent", 18, 100),
        esp_mcp_property_create_with_range("duration_ms",
                                           MOTION_MCP_MIN_DURATION_MS,
                                           MOTION_MCP_MAX_DURATION_MS),
    };
    ret = add_tool(
        mcp,
        "self.motion.spin",
        "Spin the car in place around its chassis center for an arbitrary duration. linear is always zero and the two controllable wheels run with equal and opposite speeds. Use self.motion.spin_360 for exactly one calibrated revolution. Provide all three values.",
        motion_spin_callback,
        spin_properties,
        sizeof(spin_properties) / sizeof(spin_properties[0]));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_mcp_property_t *spin_360_properties[] = {
        esp_mcp_property_create("clockwise", ESP_MCP_PROPERTY_TYPE_BOOLEAN),
    };
    ret = add_tool(
        mcp,
        "self.motion.spin_360",
        "Rotate the car exactly one calibrated revolution in place. This tool always uses linear=0, equal and opposite wheel speeds, and the firmware calibration; do not substitute self.motion.drive or choose a duration. clockwise=true rotates clockwise, false rotates counter-clockwise.",
        motion_spin_360_callback,
        spin_360_properties,
        sizeof(spin_360_properties) / sizeof(spin_360_properties[0]));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = add_tool(mcp, "self.motion.stop",
                   "Stop all car motion and release the servo output.",
                   motion_stop_callback, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = add_tool(mcp, "self.motion.get_status",
                   "Read current car, servo, motion source, and failsafe status.",
                   motion_status_callback, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = add_required_int_tool(
        mcp,
        "self.servo.set_angle",
        "Move the head to an angle from -90 to 90 degrees. Negative is down and positive is up. The head moves gently and holds for about 10 seconds.",
        servo_set_angle_callback, "angle_deg", -90, 90);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = add_tool(mcp, "self.servo.nod",
                   "Perform one gentle head nod without moving the car.",
                   servo_nod_callback, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    return add_tool(mcp, "self.servo.center",
                    "Return the head to its calibrated center position.",
                    servo_center_callback, NULL, 0);
}
