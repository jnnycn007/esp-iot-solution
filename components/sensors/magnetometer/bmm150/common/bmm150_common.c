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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "bmm150.h"
#include "bmm150_defs.h"

/******************************************************************************/
/*!                 Macro definitions                                         */

#define TAG "BMM150"

/******************************************************************************/
/*!                Static variable definition                                 */

/*! Variable that holds the I2C device address (default CSB low / SDO low = 0x10) */
static uint8_t dev_addr = BMM150_DEFAULT_I2C_ADDRESS;
static i2c_bus_handle_t i2c_bus = NULL;
static i2c_bus_device_handle_t i2c_dev = NULL;

/******************************************************************************/
/*!                User interface functions                                   */

/*!
 * I2C read function map to ESP32 platform
 */
BMM150_INTF_RET_TYPE bmm150_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    (void)intf_ptr;

    if (i2c_dev == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return BMM150_E_COM_FAIL;
    }

    ESP_LOGD(TAG, "I2C read reg 0x%02" PRIx8 " len %" PRIu32, reg_addr, length);
    esp_err_t ret = i2c_bus_read_bytes(i2c_dev, reg_addr, (uint16_t)length, reg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return BMM150_E_COM_FAIL;
    }

    return BMM150_INTF_RET_SUCCESS;
}

/*!
 * I2C write function map to ESP32 platform
 */
BMM150_INTF_RET_TYPE bmm150_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    (void)intf_ptr;

    if (i2c_dev == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return BMM150_E_COM_FAIL;
    }

    ESP_LOGD(TAG, "I2C write reg 0x%02" PRIx8 " len %" PRIu32, reg_addr, length);
    esp_err_t ret = i2c_bus_write_bytes(i2c_dev, reg_addr, (uint16_t)length, reg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(ret));
        return BMM150_E_COM_FAIL;
    }

    return BMM150_INTF_RET_SUCCESS;
}

void bmm150_delay(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;

    if (period < 1000) {
        /* Busy wait for periods <1 ms to keep micro-second accuracy */
        esp_rom_delay_us(period);
    } else {
        /* Use RTOS delay for periods ≥1 ms (rounded up) */
        vTaskDelay(pdMS_TO_TICKS((period + 999) / 1000));
    }
}

int8_t bmm150_interface_init(struct bmm150_dev *dev)
{
    int8_t rslt = BMM150_OK;

    if (dev == NULL) {
        return BMM150_E_NULL_PTR;
    }

    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL, please call bmm150_set_i2c_bus_handle first");
        return BMM150_E_COM_FAIL;
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
        return BMM150_E_COM_FAIL;
    }
    ESP_LOGI(TAG, "I2C device created at address 0x%02" PRIx8, dev_addr);

    dev->intf = BMM150_I2C_INTF;
    dev->intf_ptr = &dev_addr;
    dev->read = bmm150_i2c_read;
    dev->write = bmm150_i2c_write;
    dev->delay_us = bmm150_delay;

    return rslt;
}

void bmm150_error_codes_print_result(const char api_name[], int8_t rslt)
{
    switch (rslt) {
    case BMM150_OK:
        break;
    case BMM150_E_NULL_PTR:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Null pointer", api_name, rslt);
        break;
    case BMM150_E_COM_FAIL:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Communication fail", api_name, rslt);
        break;
    case BMM150_E_DEV_NOT_FOUND:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Device not found", api_name, rslt);
        break;
    case BMM150_E_INVALID_CONFIG:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Invalid configuration", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_YZ_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test YZ fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_XZ_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test XZ fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_Z_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test Z fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_XY_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test XY fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_Y_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test Y fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_X_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test X fail", api_name, rslt);
        break;
    case BMM150_W_NORMAL_SELF_TEST_XYZ_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Normal self-test XYZ fail", api_name, rslt);
        break;
    case BMM150_W_ADV_SELF_TEST_FAIL:
        ESP_LOGW(TAG, "API [%s] Warning [%" PRIi8 "] : Advanced self-test fail", api_name, rslt);
        break;
    default:
        ESP_LOGE(TAG, "API [%s] Error [%" PRIi8 "] : Unknown error code", api_name, rslt);
        break;
    }
}

void bmm150_set_i2c_bus_handle(i2c_bus_handle_t bus_handle)
{
    i2c_bus = bus_handle;
    ESP_LOGI(TAG, "I2C bus handle set");
}

void bmm150_set_i2c_address(uint8_t i2c_addr)
{
    dev_addr = i2c_addr;
    ESP_LOGI(TAG, "I2C address set to 0x%02" PRIx8, i2c_addr);
}

void bmm150_interface_deinit(void)
{
    ESP_LOGI(TAG, "BMM150 ESP32 deinit");

    if (i2c_dev) {
        i2c_bus_device_delete(&i2c_dev);
        i2c_dev = NULL;
    }
    /* Clear the bus handle, but do not delete the I2C bus */
    i2c_bus = NULL;
    dev_addr = BMM150_DEFAULT_I2C_ADDRESS;
}
