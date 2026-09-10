# ESP LCD Touch CST9220 Controller

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_touch_cst9220/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_touch_cst9220)

Implementation of the CST9220 touch controller with the
[esp_lcd_touch](https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch/esp_lcd_touch) component.

| Touch controller | Communication interface | Component name          |
| :--------------: | :---------------------: | :---------------------: |
|     CST9220      |           I2C           | esp_lcd_touch_cst9220   |

The driver automatically detects the report protocol used by the running firmware:

* legacy firmware uses a single report-address write and does not require an end acknowledgement;
* HYN212 firmware uses two report-address writes and an `0xAB` end acknowledgement;
* both protocols mark a release by clearing the status nibble while keeping the coordinate and the record count, so records are filtered by their touch status to keep a release from being reported as an active point;
* HYN212 coordinates are normalized to the legacy coordinate direction, so the same panel configuration works with either firmware.

The component only implements runtime touch operation. Firmware upgrade is not included.

## Add to project

Packages from this repository are uploaded to [Espressif's component service](https://components.espressif.com/).
You can add the component to a project with:

```sh
idf.py add-dependency "espressif/esp_lcd_touch_cst9220"
```

## Example use

```c
esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST9220_CONFIG();
esp_lcd_panel_io_handle_t io_handle = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_handle, &io_config, &io_handle));

const esp_lcd_touch_config_t tp_config = {
    .x_max = EXAMPLE_LCD_H_RES,
    .y_max = EXAMPLE_LCD_V_RES,
    .rst_gpio_num = EXAMPLE_TOUCH_RST,
    .int_gpio_num = EXAMPLE_TOUCH_INT,
    .levels = {
        .reset = 0,
        .interrupt = 0,
    },
};

esp_lcd_touch_handle_t tp = NULL;
ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9220(io_handle, &tp_config, &tp));
```

Read touch data through the common API:

```c
esp_lcd_touch_point_data_t points[ESP_LCD_TOUCH_CST9220_MAX_POINTS];
uint8_t point_count = 0;

ESP_ERROR_CHECK(esp_lcd_touch_read_data(tp));
ESP_ERROR_CHECK(esp_lcd_touch_get_data(tp, points, &point_count,
                                      ESP_LCD_TOUCH_CST9220_MAX_POINTS));
```

Set `CONFIG_ESP_LCD_TOUCH_MAX_POINTS` to at least `2` to receive both supported touch points.

## Sleep mode

The standard touch sleep APIs are supported:

```c
ESP_ERROR_CHECK(esp_lcd_touch_enter_sleep(tp));
ESP_ERROR_CHECK(esp_lcd_touch_exit_sleep(tp));
```

The driver sends the standard CST9220 `D105` sleep command and uses `D109` to return to normal mode. If the normal-mode
command is unavailable after sleep and a reset GPIO is configured, the driver automatically falls back to a hardware
reset. No CST9220-specific sleep API is required.

## Report acknowledgement

After each successfully read HYN212 report header, the driver writes `{0xD0, 0x00, 0xAB}`. The acknowledgement is sent
for touch, release, empty, malformed, and continuation-read failure cases so the controller does not wait for its
internal report timeout before generating the next interrupt.
