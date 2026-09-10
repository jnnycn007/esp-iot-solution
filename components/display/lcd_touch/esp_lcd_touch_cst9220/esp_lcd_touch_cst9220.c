/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_touch_cst9220.h"

#define CST9220_REG_REPORT              (0xD000)
#define CST9220_REG_REPORT_CONTINUATION (0xD007)
#define CST9220_REG_DEBUG_INFO_MODE     (0xD101)
#define CST9220_REG_SLEEP_MODE          (0xD105)
#define CST9220_REG_NORMAL_MODE         (0xD109)
#define CST9220_REG_PROTOCOL_PROBE      (0xD11E)
#define CST9220_REG_MODE_STATUS         (0x0002)
#define CST9220_REG_CHECKCODE           (0xD1FC)
#define CST9220_REG_RESOLUTION          (0xD1F8)
#define CST9220_REG_PROJECT_ID          (0xD204)

#define CST9220_CHIP_ID                 (0x9220)
#define CST9220_PROJECT_ID_LEGACY       (0x542F)
#define CST9220_PROJECT_ID_HYN212       (0x6854)
#define CST9220_REPORT_ACK              (0xAB)
#define CST9220_REPORT_HEADER_SIZE      (7)
#define CST9220_POINT_DATA_SIZE         (5)
#define CST9220_REPORT_BUFFER_SIZE      (CST9220_REPORT_HEADER_SIZE + CST9220_POINT_DATA_SIZE)
#define CST9220_PROTOCOL_PROBE_RETRIES  (3)
#define CST9220_RESET_ACTIVE_TIME_MS    (10)
#define CST9220_RESET_READY_TIME_MS     (100)
#define CST9220_MODE_SETTLE_TIME_MS     (10)
#define CST9220_REPORT_ADDRESS_DELAY_US (200)

static const char *TAG = "cst9220";

typedef enum {
    CST9220_PROTOCOL_LEGACY,
    CST9220_PROTOCOL_HYN212,
} cst9220_protocol_t;

typedef struct {
    esp_lcd_touch_t base;
    cst9220_protocol_t protocol;
    uint16_t project_id;
    uint16_t resolution_y;
    bool sleeping;
} cst9220_touch_t;

static esp_err_t cst9220_enter_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t cst9220_exit_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t cst9220_read_data(esp_lcd_touch_handle_t tp);
static bool cst9220_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                           uint8_t *point_num, uint8_t max_point_num);
static esp_err_t cst9220_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num);
static esp_err_t cst9220_del(esp_lcd_touch_handle_t tp);

static esp_err_t cst9220_write_command(esp_lcd_touch_handle_t tp, uint16_t command);
static esp_err_t cst9220_read_register(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, size_t len);
static esp_err_t cst9220_write_report_ack(esp_lcd_touch_handle_t tp);
static esp_err_t cst9220_reset(esp_lcd_touch_handle_t tp);
static esp_err_t cst9220_wake_controller(esp_lcd_touch_handle_t tp);
static esp_err_t cst9220_detect_protocol(cst9220_touch_t *cst9220);
static esp_err_t cst9220_read_identity(cst9220_touch_t *cst9220);
static esp_err_t cst9220_read_report(cst9220_touch_t *cst9220, uint8_t *report, uint8_t *point_num);

static const char *cst9220_protocol_name(cst9220_protocol_t protocol)
{
    return protocol == CST9220_PROTOCOL_HYN212 ? "HYN212" : "legacy";
}

static void cst9220_clear_data(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
}

static bool cst9220_is_sleeping(cst9220_touch_t *cst9220)
{
    bool sleeping;
    portENTER_CRITICAL(&cst9220->base.data.lock);
    sleeping = cst9220->sleeping;
    portEXIT_CRITICAL(&cst9220->base.data.lock);
    return sleeping;
}

static void cst9220_set_sleeping(cst9220_touch_t *cst9220, bool sleeping)
{
    portENTER_CRITICAL(&cst9220->base.data.lock);
    cst9220->sleeping = sleeping;
    if (sleeping) {
        cst9220->base.data.points = 0;
    }
    portEXIT_CRITICAL(&cst9220->base.data.lock);
}

esp_err_t esp_lcd_touch_new_i2c_cst9220(const esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller IO handle is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller configuration is NULL");
    ESP_RETURN_ON_FALSE(out_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller output handle is NULL");
    ESP_RETURN_ON_FALSE(config->int_gpio_num == GPIO_NUM_NC || GPIO_IS_VALID_GPIO(config->int_gpio_num),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid interrupt GPIO %d", config->int_gpio_num);
    ESP_RETURN_ON_FALSE(config->rst_gpio_num == GPIO_NUM_NC || GPIO_IS_VALID_OUTPUT_GPIO(config->rst_gpio_num),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid reset GPIO %d", config->rst_gpio_num);

    *out_touch = NULL;
    esp_err_t ret = ESP_OK;
    cst9220_touch_t *cst9220 = calloc(1, sizeof(cst9220_touch_t));
    ESP_RETURN_ON_FALSE(cst9220 != NULL, ESP_ERR_NO_MEM, TAG, "Touch controller allocation failed");

    cst9220->base.io = io;
    cst9220->base.enter_sleep = cst9220_enter_sleep;
    cst9220->base.exit_sleep = cst9220_exit_sleep;
    cst9220->base.read_data = cst9220_read_data;
    cst9220->base.get_xy = cst9220_get_xy;
    cst9220->base.get_track_id = cst9220_get_track_id;
    cst9220->base.del = cst9220_del;
    cst9220->base.data.lock.owner = portMUX_FREE_VAL;
    memcpy(&cst9220->base.config, config, sizeof(esp_lcd_touch_config_t));
    cst9220->base.config.interrupt_callback = NULL;

    if (config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = BIT64(config->int_gpio_num),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        ret = gpio_config(&int_gpio_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Interrupt GPIO configuration failed: %s", esp_err_to_name(ret));
            goto err;
        }
    }

    if (config->rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .pin_bit_mask = BIT64(config->rst_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&rst_gpio_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Reset GPIO configuration failed: %s", esp_err_to_name(ret));
            goto err;
        }
    }

    ret = cst9220_reset(&cst9220->base);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch controller reset failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = cst9220_read_identity(cst9220);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch controller identification failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = cst9220_detect_protocol(cst9220);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch protocol detection failed: %s", esp_err_to_name(ret));
        goto err;
    }

    if (config->interrupt_callback != NULL) {
        ret = esp_lcd_touch_register_interrupt_callback(&cst9220->base, config->interrupt_callback);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Interrupt callback registration failed: %s", esp_err_to_name(ret));
            cst9220->base.config.interrupt_callback = NULL;
            goto err;
        }
    }

    *out_touch = &cst9220->base;
    ESP_LOGI(TAG, "CST9220 touch controller initialized with %s protocol", cst9220_protocol_name(cst9220->protocol));
    return ESP_OK;

err: {
        esp_err_t cleanup_ret = cst9220_del(&cst9220->base);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "Touch controller cleanup failed: %s", esp_err_to_name(cleanup_ret));
        }
    }
    return ret;
}

static esp_err_t cst9220_enter_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller handle is NULL");
    cst9220_touch_t *cst9220 = (cst9220_touch_t *)tp;

    if (cst9220_is_sleeping(cst9220)) {
        return ESP_OK;
    }

    bool interrupt_disabled = false;
    esp_err_t ret;
    if (tp->config.interrupt_callback != NULL) {
        ret = gpio_intr_disable(tp->config.int_gpio_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Interrupt disable before sleep failed: %s", esp_err_to_name(ret));
            return ret;
        }
        interrupt_disabled = true;
    }

    ret = cst9220_write_command(tp, CST9220_REG_SLEEP_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enter sleep command failed: %s", esp_err_to_name(ret));
        esp_err_t wake_ret = cst9220_wake_controller(tp);
        if (wake_ret != ESP_OK) {
            ESP_LOGE(TAG, "Sleep failure recovery failed: %s", esp_err_to_name(wake_ret));
        }
        if (interrupt_disabled) {
            esp_err_t intr_ret = gpio_intr_enable(tp->config.int_gpio_num);
            if (intr_ret != ESP_OK) {
                ESP_LOGE(TAG, "Interrupt recovery failed: %s", esp_err_to_name(intr_ret));
            }
        }
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(CST9220_MODE_SETTLE_TIME_MS));
    cst9220_set_sleeping(cst9220, true);
    ESP_LOGI(TAG, "CST9220 entered sleep mode");
    return ESP_OK;
}

static esp_err_t cst9220_exit_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller handle is NULL");
    cst9220_touch_t *cst9220 = (cst9220_touch_t *)tp;

    if (!cst9220_is_sleeping(cst9220)) {
        return ESP_OK;
    }

    esp_err_t ret = cst9220_wake_controller(tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wakeup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (tp->config.interrupt_callback != NULL) {
        ret = gpio_intr_enable(tp->config.int_gpio_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Interrupt enable after wakeup failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    cst9220_set_sleeping(cst9220, false);
    ESP_LOGI(TAG, "CST9220 exited sleep mode");
    return ESP_OK;
}

static esp_err_t cst9220_read_data(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller handle is NULL");
    cst9220_touch_t *cst9220 = (cst9220_touch_t *)tp;
    ESP_RETURN_ON_FALSE(!cst9220_is_sleeping(cst9220), ESP_ERR_INVALID_STATE, TAG,
                        "Touch controller is sleeping");

    uint8_t report[CST9220_REPORT_BUFFER_SIZE] = {0};
    uint8_t reported_points = 0;
    esp_err_t ret = cst9220_read_report(cst9220, report, &reported_points);
    if (ret != ESP_OK) {
        cst9220_clear_data(tp);
        return ret;
    }

    esp_lcd_touch_point_data_t decoded[ESP_LCD_TOUCH_CST9220_MAX_POINTS] = {0};
    uint8_t decoded_points = 0;
    const uint8_t *point = report;
    for (uint8_t i = 0; i < reported_points; i++) {
        /* Both protocols keep the coordinate and the record count on the
         * release record and only clear the status nibble, so an unfiltered
         * record would hold the last coordinate pressed forever. */
        const uint8_t status = point[0] & 0x0F;
        const bool active = (status >> 1) == 3;
        if (active) {
            const uint16_t raw_y = ((uint16_t)point[2] << 4) | (point[3] & 0x0F);
            if (cst9220->protocol == CST9220_PROTOCOL_HYN212 && raw_y > cst9220->resolution_y) {
                ESP_LOGW(TAG, "Ignoring out-of-range HYN212 Y coordinate %u", raw_y);
            } else {
                decoded[decoded_points].track_id = point[0] >> 4;
                decoded[decoded_points].x = ((uint16_t)point[1] << 4) | (point[3] >> 4);
                decoded[decoded_points].y = cst9220->protocol == CST9220_PROTOCOL_LEGACY ?
                                            raw_y : (uint16_t)(cst9220->resolution_y - raw_y);
                decoded[decoded_points].strength = 1;
                decoded_points++;
            }
        }
        point += i == 0 ? CST9220_REPORT_HEADER_SIZE : CST9220_POINT_DATA_SIZE;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = decoded_points > CONFIG_ESP_LCD_TOUCH_MAX_POINTS ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : decoded_points;
    for (uint8_t i = 0; i < tp->data.points; i++) {
        tp->data.coords[i] = decoded[i];
    }
    portEXIT_CRITICAL(&tp->data.lock);

    ESP_LOGD(TAG, "Report decoded: protocol=%s records=%u active=%u",
             cst9220_protocol_name(cst9220->protocol), reported_points, decoded_points);
    return ESP_OK;
}

static bool cst9220_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                           uint8_t *point_num, uint8_t max_point_num)
{
    ESP_RETURN_ON_FALSE(tp != NULL, false, TAG, "Touch controller handle is NULL");
    ESP_RETURN_ON_FALSE(x != NULL, false, TAG, "X coordinate output is NULL");
    ESP_RETURN_ON_FALSE(y != NULL, false, TAG, "Y coordinate output is NULL");
    ESP_RETURN_ON_FALSE(point_num != NULL, false, TAG, "Point count output is NULL");
    ESP_RETURN_ON_FALSE(max_point_num > 0, false, TAG, "Point capacity is zero");

    portENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (uint8_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != NULL) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return *point_num > 0;
}

static esp_err_t cst9220_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller handle is NULL");
    ESP_RETURN_ON_FALSE(track_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Track ID output is NULL");
    ESP_RETURN_ON_FALSE(point_num > 0, ESP_ERR_INVALID_ARG, TAG, "Track ID count is zero");

    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&tp->data.lock);
    if (point_num > tp->data.points) {
        ret = ESP_ERR_INVALID_SIZE;
    } else {
        for (uint8_t i = 0; i < point_num; i++) {
            track_id[i] = tp->data.coords[i].track_id;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Track ID count %u exceeds active points: %s", point_num, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t cst9220_del(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "Touch controller handle is NULL");
    esp_err_t first_error = ESP_OK;

    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        if (tp->config.interrupt_callback != NULL) {
            esp_err_t ret = gpio_intr_disable(tp->config.int_gpio_num);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Interrupt disable during delete failed: %s", esp_err_to_name(ret));
                first_error = ret;
            }
            ret = gpio_isr_handler_remove(tp->config.int_gpio_num);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Interrupt handler removal failed; touch handle retained: %s", esp_err_to_name(ret));
                return ret;
            }
        }
        esp_err_t ret = gpio_reset_pin(tp->config.int_gpio_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Interrupt GPIO reset failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        esp_err_t ret = gpio_reset_pin(tp->config.rst_gpio_num);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Reset GPIO release failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    free(tp);
    ESP_LOGI(TAG, "CST9220 touch controller deleted");
    return first_error;
}

static esp_err_t cst9220_write_command(esp_lcd_touch_handle_t tp, uint16_t command)
{
    const uint8_t parameter = (uint8_t)command;
    return esp_lcd_panel_io_tx_param(tp->io, command >> 8, &parameter, sizeof(parameter));
}

static esp_err_t cst9220_read_register(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid register read buffer");

    esp_err_t ret = cst9220_write_command(tp, reg);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_lcd_panel_io_rx_param(tp->io, -1, data, len);
}

static esp_err_t cst9220_write_report_ack(esp_lcd_touch_handle_t tp)
{
    const uint8_t parameters[] = {(uint8_t)CST9220_REG_REPORT, CST9220_REPORT_ACK};
    return esp_lcd_panel_io_tx_param(tp->io, CST9220_REG_REPORT >> 8, parameters, sizeof(parameters));
}

static esp_err_t cst9220_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }

    esp_err_t ret = gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(CST9220_RESET_ACTIVE_TIME_MS));
    ret = gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(CST9220_RESET_READY_TIME_MS));
    return ESP_OK;
}

static esp_err_t cst9220_wake_controller(esp_lcd_touch_handle_t tp)
{
    cst9220_touch_t *cst9220 = (cst9220_touch_t *)tp;
    if (cst9220->project_id == CST9220_PROJECT_ID_LEGACY && tp->config.rst_gpio_num != GPIO_NUM_NC) {
        return cst9220_reset(tp);
    }

    esp_err_t ret = cst9220_write_command(tp, CST9220_REG_NORMAL_MODE);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CST9220_MODE_SETTLE_TIME_MS));
        return ESP_OK;
    }

    if (tp->config.rst_gpio_num == GPIO_NUM_NC) {
        return ret;
    }

    ESP_LOGW(TAG, "Normal mode command failed (%s), falling back to hardware reset", esp_err_to_name(ret));
    return cst9220_reset(tp);
}

static esp_err_t cst9220_detect_protocol(cst9220_touch_t *cst9220)
{
    esp_lcd_touch_handle_t tp = &cst9220->base;

    if (cst9220->project_id == CST9220_PROJECT_ID_LEGACY) {
        cst9220->protocol = CST9220_PROTOCOL_LEGACY;
        return ESP_OK;
    }
    if (cst9220->project_id == CST9220_PROJECT_ID_HYN212) {
        cst9220->protocol = CST9220_PROTOCOL_HYN212;
        return ESP_OK;
    }

    esp_err_t last_error = ESP_ERR_INVALID_RESPONSE;

    for (uint8_t attempt = 0; attempt < CST9220_PROTOCOL_PROBE_RETRIES; attempt++) {
        uint8_t mode[2] = {0};
        last_error = cst9220_write_command(tp, CST9220_REG_PROTOCOL_PROBE);
        if (last_error == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            last_error = cst9220_read_register(tp, CST9220_REG_MODE_STATUS, mode, sizeof(mode));
        }
        if (last_error == ESP_OK && mode[1] == (uint8_t)CST9220_REG_PROTOCOL_PROBE) {
            last_error = cst9220_wake_controller(tp);
            if (last_error != ESP_OK) {
                return last_error;
            }
            cst9220->protocol = CST9220_PROTOCOL_HYN212;
            return ESP_OK;
        }
        if (last_error == ESP_OK) {
            last_error = ESP_ERR_INVALID_RESPONSE;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    esp_err_t restore_ret = cst9220_wake_controller(tp);
    if (restore_ret != ESP_OK) {
        return restore_ret;
    }
    cst9220->protocol = CST9220_PROTOCOL_LEGACY;
    ESP_LOGW(TAG, "HYN212 protocol probe did not match (%s); using legacy report protocol",
             esp_err_to_name(last_error));
    return ESP_OK;
}

static esp_err_t cst9220_read_identity(cst9220_touch_t *cst9220)
{
    esp_lcd_touch_handle_t tp = &cst9220->base;
    esp_err_t ret = cst9220_write_command(tp, CST9220_REG_DEBUG_INFO_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Debug information mode entry failed: %s", esp_err_to_name(ret));
        goto exit_mode;
    }
    vTaskDelay(pdMS_TO_TICKS(CST9220_MODE_SETTLE_TIME_MS));

    uint8_t data[4] = {0};
    ret = cst9220_read_register(tp, CST9220_REG_CHECKCODE, data, sizeof(data));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Checkcode read failed: %s", esp_err_to_name(ret));
        goto exit_mode;
    }
    const uint32_t checkcode = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);

    ret = cst9220_read_register(tp, CST9220_REG_RESOLUTION, data, sizeof(data));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Resolution read failed: %s", esp_err_to_name(ret));
        goto exit_mode;
    }
    const uint16_t resolution_x = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    const uint16_t resolution_y = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    if (resolution_x == 0 || resolution_y == 0) {
        ESP_LOGE(TAG, "Invalid controller resolution %ux%u: %s", resolution_x, resolution_y,
                 esp_err_to_name(ESP_ERR_INVALID_RESPONSE));
        ret = ESP_ERR_INVALID_RESPONSE;
        goto exit_mode;
    }

    ret = cst9220_read_register(tp, CST9220_REG_PROJECT_ID, data, sizeof(data));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Project information read failed: %s", esp_err_to_name(ret));
        goto exit_mode;
    }
    const uint16_t project_id = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    const uint16_t chip_id = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    if (chip_id != CST9220_CHIP_ID) {
        ESP_LOGE(TAG, "Unsupported touch controller chip ID 0x%04x: %s", chip_id,
                 esp_err_to_name(ESP_ERR_NOT_SUPPORTED));
        ret = ESP_ERR_NOT_SUPPORTED;
        goto exit_mode;
    }
    cst9220->project_id = project_id;
    cst9220->resolution_y = resolution_y;

    ESP_LOGI(TAG, "Controller identified: chip=0x%04x project=0x%04x resolution=%ux%u checkcode=0x%08" PRIx32,
             chip_id, project_id, resolution_x, resolution_y, checkcode);

exit_mode: {
        esp_err_t exit_ret = cst9220_write_command(tp, CST9220_REG_NORMAL_MODE);
        if (exit_ret != ESP_OK) {
            ESP_LOGE(TAG, "Normal mode entry failed: %s", esp_err_to_name(exit_ret));
            if (ret == ESP_OK) {
                ret = exit_ret;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(CST9220_MODE_SETTLE_TIME_MS));
        }
    }
    return ret;
}

static esp_err_t cst9220_read_report(cst9220_touch_t *cst9220, uint8_t *report, uint8_t *point_num)
{
    esp_lcd_touch_handle_t tp = &cst9220->base;
    esp_err_t ret = cst9220_write_command(tp, CST9220_REG_REPORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Report address write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (cst9220->protocol == CST9220_PROTOCOL_HYN212) {
        esp_rom_delay_us(CST9220_REPORT_ADDRESS_DELAY_US);
        ret = cst9220_write_command(tp, CST9220_REG_REPORT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Second HYN212 report address write failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = esp_lcd_panel_io_rx_param(tp->io, -1, report, CST9220_REPORT_HEADER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Report header read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    bool all_zero = true;
    for (uint8_t i = 0; i < CST9220_REPORT_HEADER_SIZE; i++) {
        if (report[i] != 0) {
            all_zero = false;
            break;
        }
    }

    esp_err_t report_ret = ESP_OK;
    uint8_t reported_points = 0;
    if (!all_zero) {
        if (report[0] == CST9220_REPORT_ACK || report[6] != CST9220_REPORT_ACK) {
            ESP_LOGW(TAG, "Malformed report header: first=0x%02x marker=0x%02x", report[0], report[6]);
            report_ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            reported_points = report[5] & 0x7F;
            if (reported_points > ESP_LCD_TOUCH_CST9220_MAX_POINTS) {
                ESP_LOGW(TAG, "Invalid report point count %u", reported_points);
                report_ret = ESP_ERR_INVALID_RESPONSE;
                reported_points = 0;
            }
        }
    }

    if (report_ret == ESP_OK && reported_points > 1) {
        ret = cst9220_read_register(tp, CST9220_REG_REPORT_CONTINUATION,
                                    &report[CST9220_REPORT_HEADER_SIZE], CST9220_POINT_DATA_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Report continuation read failed: %s", esp_err_to_name(ret));
            report_ret = ret;
            reported_points = 0;
        }
    }

    if (cst9220->protocol == CST9220_PROTOCOL_HYN212) {
        ret = cst9220_write_report_ack(tp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HYN212 report ACK failed: %s", esp_err_to_name(ret));
            report_ret = ret;
        }
    }

    *point_num = report_ret == ESP_OK ? reported_points : 0;
    return report_ret;
}
