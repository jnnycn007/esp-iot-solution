/**
 * Copyright (C) 2023 Bosch Sensortec GmbH.
 * Copyright (C) 2025-2026 Espressif Systems (Shanghai) CO LTD.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "i2c_bus.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "bmm350.h"
#include "bmm350_defs.h"

/******************************************************************************/
/*!                 Macro definitions                                         */

#define TAG "BMM350"

/******************************************************************************/
/*!                Static variable definition                                 */

/*! Variable that holds the I2C device address (default ADSEL low = 0x14) */
static uint8_t dev_addr = BMM350_I2C_ADSEL_SET_LOW;
static i2c_bus_handle_t i2c_bus = NULL;
static i2c_bus_device_handle_t i2c_dev = NULL;

/******************************************************************************/
/*!                User interface functions                                   */

/*!
 * I2C read function map to ESP32 platform
 */
BMM350_INTF_RET_TYPE bmm350_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    (void)intf_ptr;

    if (i2c_dev == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return BMM350_E_COM_FAIL;
    }

    ESP_LOGD(TAG, "I2C read reg 0x%02" PRIx8 " len %" PRIu32, reg_addr, length);
    esp_err_t ret = i2c_bus_read_bytes(i2c_dev, reg_addr, (uint16_t)length, reg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return BMM350_E_COM_FAIL;
    }

    return BMM350_INTF_RET_SUCCESS;
}

/*!
 * I2C write function map to ESP32 platform
 */
BMM350_INTF_RET_TYPE bmm350_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    (void)intf_ptr;

    if (i2c_dev == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return BMM350_E_COM_FAIL;
    }

    ESP_LOGD(TAG, "I2C write reg 0x%02" PRIx8 " len %" PRIu32, reg_addr, length);
    esp_err_t ret = i2c_bus_write_bytes(i2c_dev, reg_addr, (uint16_t)length, reg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(ret));
        return BMM350_E_COM_FAIL;
    }

    return BMM350_INTF_RET_SUCCESS;
}

void bmm350_delay(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;

    if (period == 0) {
        return;
    }

    int64_t start_us = esp_timer_get_time();
    uint64_t full_ticks = ((uint64_t)period * configTICK_RATE_HZ) / 1000000ULL;

    /*
     * vTaskDelay() wakes on tick boundaries and can therefore block for up
     * to one tick less than expected. Measure the actual elapsed time below
     * and busy-wait only for the remaining microseconds.
     */
    if (full_ticks > 0) {
        vTaskDelay((TickType_t)full_ticks);
    }

    int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (elapsed_us < period) {
        esp_rom_delay_us(period - (uint32_t)elapsed_us);
    }
}

int8_t bmm350_interface_init(struct bmm350_dev *dev)
{
    int8_t rslt = BMM350_OK;

    if (dev == NULL) {
        return BMM350_E_NULL_PTR;
    }

    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL, please call bmm350_set_i2c_bus_handle first");
        return BMM350_E_COM_FAIL;
    }

    ESP_LOGI(TAG, "I2C Interface, dev_addr=0x%02" PRIx8, dev_addr);

    /* Allow re-init (e.g. address probe) by replacing the previous device handle */
    if (i2c_dev != NULL) {
        i2c_bus_device_delete(&i2c_dev);
        i2c_dev = NULL;
    }

    i2c_dev = i2c_bus_device_create(i2c_bus, dev_addr, 0);
    if (i2c_dev == NULL) {
        ESP_LOGE(TAG, "i2c_bus_device_create failed");
        return BMM350_E_COM_FAIL;
    }
    ESP_LOGI(TAG, "I2C device created at address 0x%02" PRIx8, dev_addr);

    dev->intf_ptr = &dev_addr;
    dev->read = bmm350_i2c_read;
    dev->write = bmm350_i2c_write;
    dev->delay_us = bmm350_delay;

    return rslt;
}

void bmm350_error_codes_print_result(const char api_name[], int8_t rslt)
{
    switch (rslt) {
    case BMM350_OK:
        break;
    case BMM350_E_NULL_PTR:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Null pointer", api_name, rslt);
        break;
    case BMM350_E_COM_FAIL:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Communication fail", api_name, rslt);
        break;
    case BMM350_E_DEV_NOT_FOUND:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Device not found", api_name, rslt);
        break;
    case BMM350_E_INVALID_CONFIG:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Invalid configuration", api_name, rslt);
        break;
    case BMM350_E_BAD_PAD_DRIVE:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Bad pad drive", api_name, rslt);
        break;
    case BMM350_E_RESET_UNFINISHED:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Reset unfinished", api_name, rslt);
        break;
    case BMM350_E_INVALID_INPUT:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Invalid input", api_name, rslt);
        break;
    case BMM350_E_SELF_TEST_INVALID_AXIS:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Self-test invalid axis selection", api_name, rslt);
        break;
    case BMM350_E_OTP_BOOT:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP boot", api_name, rslt);
        break;
    case BMM350_E_OTP_PAGE_RD:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP page read", api_name, rslt);
        break;
    case BMM350_E_OTP_PAGE_PRG:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP page prog", api_name, rslt);
        break;
    case BMM350_E_OTP_SIGN:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP sign", api_name, rslt);
        break;
    case BMM350_E_OTP_INV_CMD:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP invalid command", api_name, rslt);
        break;
    case BMM350_E_OTP_UNDEFINED:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : OTP undefined", api_name, rslt);
        break;
    case BMM350_E_ALL_AXIS_DISABLED:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : All axis are disabled", api_name, rslt);
        break;
    case BMM350_E_PMU_CMD_VALUE:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Unexpected PMU CMD value", api_name, rslt);
        break;
    default:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Unknown error code", api_name, rslt);
        break;
    }
}

void bmm350_set_i2c_bus_handle(i2c_bus_handle_t bus_handle)
{
    i2c_bus = bus_handle;
    ESP_LOGI(TAG, "I2C bus handle set");
}

void bmm350_set_i2c_address(uint8_t i2c_addr)
{
    dev_addr = i2c_addr;
    ESP_LOGI(TAG, "I2C address set to 0x%02" PRIx8, i2c_addr);
}

void bmm350_interface_deinit(void)
{
    ESP_LOGI(TAG, "BMM350 ESP32 deinit");

    if (i2c_dev) {
        i2c_bus_device_delete(&i2c_dev);
        i2c_dev = NULL;
    }
    // and clear the bus handle, but not delete the I2C bus
    i2c_bus = NULL;
    dev_addr = BMM350_I2C_ADSEL_SET_LOW;
}
