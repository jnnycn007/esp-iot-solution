# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "src/esp_lv_present.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_display_present lvgl
    PRIV_REQUIRES esp_timer
)
