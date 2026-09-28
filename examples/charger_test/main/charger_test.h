#pragma once

#include <cstdint>

#include "esp_err.h"

enum class ChargePhase : uint8_t {
    kNotCharging = 0,
    kTrickle = 1,
    kPreCharge = 2,
    kFastCharge = 3,
    kTaperCharge = 4,
    kTopOff = 5,
    kDone = 6,
    kUnknown = 7,
};

enum class InputSource : uint8_t {
    kNone = 0,
    kUsbSdp = 1,
    kUsbCdp = 2,
    kUsbDcp = 3,
    kPoorSource = 4,
    kUnknownAdapter = 5,
    kNonStandard = 6,
    kOtg = 7,
};

enum class NtcState : uint8_t {
    kNormal = 0,
    kWarm = 2,
    kCool = 3,
    kCold = 5,
    kHot = 6,
    kUnknown = 7,
};

struct ChargerTestState {
    bool data_valid = false;
    bool device_present = false;
    bool part_id_valid = false;
    bool configuration_valid = false;
    bool charge_enabled = false;
    bool charge_pin_enabled = false;
    bool register_charge_enabled = false;
    bool high_impedance_mode = false;
    bool battery_voltage_safe = false;
    bool single_cell_suspected = false;
    bool adc_enabled = false;
    bool power_good = false;
    bool power_good_pin = false;
    bool interrupt_pin_active = false;
    bool input_current_regulation = false;
    bool input_voltage_regulation = false;
    bool thermal_regulation = false;
    bool watchdog_expired = false;
    bool minimum_system_regulation = false;

    uint8_t part_id = 0;
    uint8_t revision = 0;
    uint8_t fault_bits = 0;
    uint8_t ico_status = 0;

    ChargePhase phase = ChargePhase::kUnknown;
    InputSource input_source = InputSource::kNone;
    NtcState ntc_state = NtcState::kUnknown;

    int vbus_mv = 0;
    int ibus_ma = 0;
    int vbat_mv = 0;
    int ichg_ma = 0;
    int vsys_mv = 0;
    int die_temperature_deci_c = 0;
    int ts_deci_percent = 0;

    int voltage_limit_mv = 0;
    int charge_current_limit_ma = 0;
    int input_current_limit_ma = 0;
    int precharge_current_ma = 0;
    int termination_current_ma = 0;
    int thermal_regulation_limit_c = 0;

    char detail[96] = {};
};

using ChargerStateCallback =
    void (*)(const ChargerTestState &state, void *context);

esp_err_t charger_test_start(
    ChargerStateCallback callback,
    void *callback_context);
