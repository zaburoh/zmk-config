/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_pmw3360

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum pmw3360_reg {
    PMW3360_PRODUCT_ID = 0x00,
    PMW3360_REVISION_ID = 0x01,
    PMW3360_MOTION = 0x02,
    PMW3360_DELTA_X_L = 0x03,
    PMW3360_DELTA_X_H = 0x04,
    PMW3360_DELTA_Y_L = 0x05,
    PMW3360_DELTA_Y_H = 0x06,
    PMW3360_CONFIG1 = 0x0F,
    PMW3360_CONFIG2 = 0x10,
    PMW3360_POWER_UP_RESET = 0x3A,
};

struct pmw3360_config {
    struct spi_dt_spec spi;
    uint16_t polling_interval_ms;
    uint16_t cpi;
};

struct pmw3360_data {
    const struct device *dev;
    struct k_work_delayable work;
};

static struct spi_config pmw3360_spi_hold_cfg(const struct pmw3360_config *cfg) {
    struct spi_config spi_cfg = cfg->spi.config;
    spi_cfg.operation |= SPI_HOLD_ON_CS;
    return spi_cfg;
}

static int pmw3360_spi_write_hold(const struct pmw3360_config *cfg, const uint8_t *buf, size_t len) {
    struct spi_buf tx_buf = {
        .buf = (void *)buf,
        .len = len,
    };
    const struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };
    struct spi_config spi_cfg = pmw3360_spi_hold_cfg(cfg);

    return spi_write(cfg->spi.bus, &spi_cfg, &tx);
}

static int pmw3360_spi_read_hold(const struct pmw3360_config *cfg, uint8_t *buf, size_t len) {
    struct spi_buf rx_buf = {
        .buf = buf,
        .len = len,
    };
    const struct spi_buf_set rx = {
        .buffers = &rx_buf,
        .count = 1,
    };
    struct spi_config spi_cfg = pmw3360_spi_hold_cfg(cfg);

    return spi_read(cfg->spi.bus, &spi_cfg, &rx);
}

static void pmw3360_spi_release_hold(const struct pmw3360_config *cfg) {
    struct spi_config spi_cfg = pmw3360_spi_hold_cfg(cfg);
    (void)spi_release(cfg->spi.bus, &spi_cfg);
}

static int pmw3360_reg_read(const struct pmw3360_config *cfg, uint8_t reg, uint8_t *val) {
    uint8_t addr = reg & 0x7F;
    int err = pmw3360_spi_write_hold(cfg, &addr, sizeof(addr));
    if (err) {
        pmw3360_spi_release_hold(cfg);
        return err;
    }

    k_busy_wait(160);

    uint8_t data = 0;
    err = pmw3360_spi_read_hold(cfg, &data, sizeof(data));
    pmw3360_spi_release_hold(cfg);
    k_busy_wait(19);

    if (err == 0) {
        *val = data;
    }

    return err;
}

static int pmw3360_reg_write(const struct pmw3360_config *cfg, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), val };
    int err = pmw3360_spi_write_hold(cfg, tx, sizeof(tx));
    k_busy_wait(35);
    pmw3360_spi_release_hold(cfg);
    k_busy_wait(145);
    return err;
}

static int pmw3360_set_cpi(const struct pmw3360_config *cfg) {
    uint16_t cpi = cfg->cpi;
    if (cpi < 100) {
        cpi = 100;
    } else if (cpi > 12000) {
        cpi = 12000;
    }

    uint8_t reg_val = (uint8_t)((cpi / 100) - 1);
    return pmw3360_reg_write(cfg, PMW3360_CONFIG1, reg_val);
}

static bool pmw3360_read_motion(const struct pmw3360_config *cfg, int16_t *dx, int16_t *dy) {
    uint8_t motion = 0;
    if (pmw3360_reg_read(cfg, PMW3360_MOTION, &motion) != 0) {
        return false;
    }

    if ((motion & 0x80) == 0) {
        return false;
    }

    uint8_t xl = 0;
    uint8_t xh = 0;
    uint8_t yl = 0;
    uint8_t yh = 0;

    if (pmw3360_reg_read(cfg, PMW3360_DELTA_X_L, &xl) != 0 ||
        pmw3360_reg_read(cfg, PMW3360_DELTA_X_H, &xh) != 0 ||
        pmw3360_reg_read(cfg, PMW3360_DELTA_Y_L, &yl) != 0 ||
        pmw3360_reg_read(cfg, PMW3360_DELTA_Y_H, &yh) != 0) {
        return false;
    }

    *dx = (int16_t)((xh << 8) | xl);
    *dy = (int16_t)((yh << 8) | yl);

    return true;
}

static void pmw3360_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct pmw3360_data *data = CONTAINER_OF(dwork, struct pmw3360_data, work);
    const struct device *dev = data->dev;
    const struct pmw3360_config *cfg = dev->config;

    int16_t dx = 0;
    int16_t dy = 0;

    if (pmw3360_read_motion(cfg, &dx, &dy) && (dx != 0 || dy != 0)) {
        input_report(dev, INPUT_EV_REL, INPUT_REL_X, dx, false, K_NO_WAIT);
        input_report(dev, INPUT_EV_REL, INPUT_REL_Y, dy, true, K_NO_WAIT);
    }

    k_work_schedule(&data->work, K_MSEC(cfg->polling_interval_ms));
}

static int pmw3360_init(const struct device *dev) {
    struct pmw3360_data *data = dev->data;
    const struct pmw3360_config *cfg = dev->config;

    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("PMW3360 SPI bus not ready");
        return -ENODEV;
    }

    if (pmw3360_reg_write(cfg, PMW3360_POWER_UP_RESET, 0x5A) != 0) {
        LOG_ERR("PMW3360 reset failed");
        return -EIO;
    }

    k_msleep(50);

    uint8_t pid = 0;
    uint8_t rev = 0;
    if (pmw3360_reg_read(cfg, PMW3360_PRODUCT_ID, &pid) != 0 ||
        pmw3360_reg_read(cfg, PMW3360_REVISION_ID, &rev) != 0) {
        LOG_ERR("PMW3360 ID read failed");
        return -EIO;
    }

    if (pid != 0x42 || rev != 0x01) {
        LOG_ERR("PMW3360 unexpected ID: 0x%02x/0x%02x", pid, rev);
        return -ENODEV;
    }

    (void)pmw3360_reg_write(cfg, PMW3360_CONFIG2, 0x00);
    (void)pmw3360_set_cpi(cfg);

    data->dev = dev;
    k_work_init_delayable(&data->work, pmw3360_work_handler);
    k_work_schedule(&data->work, K_MSEC(cfg->polling_interval_ms));

    return 0;
}

#define PMW3360_INST(n)                                                                           \
    static struct pmw3360_data pmw3360_data_##n = {};                                              \
    static const struct pmw3360_config pmw3360_config_##n = {                                      \
        .spi = SPI_DT_SPEC_INST_GET(n, SPI_OP_MODE_MASTER | SPI_MODE_CPOL | SPI_MODE_CPHA |        \
                                            SPI_WORD_SET(8) | SPI_TRANSFER_MSB,                   \
                                    0),                                                           \
        .polling_interval_ms = DT_INST_PROP_OR(n, polling_interval_ms, 4),                         \
        .cpi = DT_INST_PROP_OR(n, cpi, 500),                                                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, pmw3360_init, NULL, &pmw3360_data_##n, &pmw3360_config_##n,            \
                          POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PMW3360_INST)
