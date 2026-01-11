/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_joystick_analog

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(zmk_joystick_analog, CONFIG_ZMK_LOG_LEVEL);

struct joystick_config {
    struct adc_dt_spec x_adc;
    struct adc_dt_spec y_adc;
    uint16_t poll_interval_ms;
    uint16_t deadzone;
    uint16_t scale_divisor;
    bool invert_x;
    bool invert_y;
};

struct joystick_data {
    const struct device *dev;
    struct k_work_delayable work;
    int32_t center_x;
    int32_t center_y;
};

static uint16_t poll_interval_or_default(uint16_t interval_ms) {
    return interval_ms > 0 ? interval_ms : 10;
}

static int joystick_read_axis(const struct adc_dt_spec *spec, int16_t *out) {
    int16_t buf = 0;
    struct adc_sequence sequence = {
        .buffer = &buf,
        .buffer_size = sizeof(buf),
    };
    int err = adc_sequence_init_dt(spec, &sequence);
    if (err < 0) {
        return err;
    }

    err = adc_read(spec->dev, &sequence);
    if (err < 0) {
        return err;
    }

    *out = buf;
    return 0;
}

static int joystick_calibrate(const struct joystick_config *cfg, struct joystick_data *data) {
    int64_t sum_x = 0;
    int64_t sum_y = 0;

    for (int i = 0; i < 8; i++) {
        int16_t raw_x = 0;
        int16_t raw_y = 0;
        int err = joystick_read_axis(&cfg->x_adc, &raw_x);
        if (err < 0) {
            return err;
        }
        err = joystick_read_axis(&cfg->y_adc, &raw_y);
        if (err < 0) {
            return err;
        }
        sum_x += raw_x;
        sum_y += raw_y;
        k_sleep(K_MSEC(2));
    }

    data->center_x = (int32_t)(sum_x / 8);
    data->center_y = (int32_t)(sum_y / 8);
    return 0;
}

static int32_t apply_deadzone(int32_t value, uint16_t deadzone) {
    if (value > 0) {
        if (value <= deadzone) {
            return 0;
        }
        return value - deadzone;
    }
    if (value < 0) {
        if (-value <= deadzone) {
            return 0;
        }
        return value + deadzone;
    }
    return 0;
}

static int32_t scale_value(int32_t value, uint16_t divisor) {
    uint16_t safe_divisor = divisor == 0 ? 1 : divisor;
    return value / (int32_t)safe_divisor;
}

static void joystick_report(const struct device *dev, int32_t dx, int32_t dy) {
    bool have_x = dx != 0;
    bool have_y = dy != 0;

    if (have_x) {
        input_report_rel(dev, INPUT_REL_X, (int16_t)CLAMP(dx, INT16_MIN, INT16_MAX), !have_y,
                         K_NO_WAIT);
    }
    if (have_y) {
        input_report_rel(dev, INPUT_REL_Y, (int16_t)CLAMP(dy, INT16_MIN, INT16_MAX), true,
                         K_NO_WAIT);
    }
}

static void joystick_work_cb(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct joystick_data *data = CONTAINER_OF(d_work, struct joystick_data, work);
    const struct device *dev = data->dev;
    const struct joystick_config *cfg = dev->config;

    int16_t raw_x = 0;
    int16_t raw_y = 0;
    if (joystick_read_axis(&cfg->x_adc, &raw_x) < 0 ||
        joystick_read_axis(&cfg->y_adc, &raw_y) < 0) {
        k_work_schedule(&data->work, K_MSEC(poll_interval_or_default(cfg->poll_interval_ms)));
        return;
    }

    int32_t dx = raw_x - data->center_x;
    int32_t dy = raw_y - data->center_y;

    dx = scale_value(apply_deadzone(dx, cfg->deadzone), cfg->scale_divisor);
    dy = scale_value(apply_deadzone(dy, cfg->deadzone), cfg->scale_divisor);

    if (cfg->invert_x) {
        dx = -dx;
    }
    if (cfg->invert_y) {
        dy = -dy;
    }

    joystick_report(dev, dx, dy);
    k_work_schedule(&data->work, K_MSEC(poll_interval_or_default(cfg->poll_interval_ms)));
}

static int joystick_init(const struct device *dev) {
    const struct joystick_config *cfg = dev->config;
    struct joystick_data *data = dev->data;

    if (!adc_is_ready_dt(&cfg->x_adc) || !adc_is_ready_dt(&cfg->y_adc)) {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }

    int err = adc_channel_setup_dt(&cfg->x_adc);
    if (err < 0) {
        LOG_ERR("Failed to setup X axis ADC channel (%d)", err);
        return err;
    }
    err = adc_channel_setup_dt(&cfg->y_adc);
    if (err < 0) {
        LOG_ERR("Failed to setup Y axis ADC channel (%d)", err);
        return err;
    }

    err = joystick_calibrate(cfg, data);
    if (err < 0) {
        LOG_ERR("Failed to calibrate joystick (%d)", err);
        return err;
    }

    data->dev = dev;
    k_work_init_delayable(&data->work, joystick_work_cb);
    k_work_schedule(&data->work, K_MSEC(poll_interval_or_default(cfg->poll_interval_ms)));

    return 0;
}

#define JOYSTICK_ANALOG_INST(n)                                                                    \
    static struct joystick_data joystick_data_##n = {};                                            \
    static const struct joystick_config joystick_config_##n = {                                    \
        .x_adc = ADC_DT_SPEC_GET_BY_IDX(DT_DRV_INST(n), 0),                                         \
        .y_adc = ADC_DT_SPEC_GET_BY_IDX(DT_DRV_INST(n), 1),                                         \
        .poll_interval_ms = DT_PROP_OR(DT_DRV_INST(n), poll_interval_ms, 10),                       \
        .deadzone = DT_PROP_OR(DT_DRV_INST(n), deadzone, 100),                                      \
        .scale_divisor = DT_PROP_OR(DT_DRV_INST(n), scale_divisor, 128),                            \
        .invert_x = DT_PROP_OR(DT_DRV_INST(n), invert_x, 0),                                        \
        .invert_y = DT_PROP_OR(DT_DRV_INST(n), invert_y, 0),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, joystick_init, NULL, &joystick_data_##n, &joystick_config_##n,        \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(JOYSTICK_ANALOG_INST)
