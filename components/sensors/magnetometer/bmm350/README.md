# BMM350 SensorAPI

> **Bosch Sensortec's [BMM350](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmm350-ds001.pdf) SensorAPI**
>
> This driver component is based on the Bosch Sensortec's [BMM350_SensorAPI](https://github.com/boschsensortec/BMM350_SensorAPI) v1.4.0,
> with modifications to `common/*` to adapt to the ESP platform.

## Sensor Overview

The BMM350 is a 3-axis magnetic sensor which operates in automatic mode or triggered mode.
The magnetic-to-digital conversion technology is based on TMR (tunnel magneto resistance).
The BMM350 has an excellent temperature behaviour with an outstanding low temperature coefficient of the offset (TCO) and temperature coefficient of the sensitivity (TCS).

### Applications

1. Virtual, augmented and mixed reality applications
2. High-end gaming applications
3. Platform stabilization applications such as image stabilization, or indoor navigation and dead-reckoning, for example in robotics applications.
4. Magnetic heading information
5. Tilt-compensated electronic compass for map rotation, navigation and augmented reality
6. Gyroscope calibration in 9-DoF applications for mobile devices
7. In-door navigation, e.g. step counting in combination with accelerometer
8. Gaming (AR/VR)

## Direct I2C

Bind the ESP platform callbacks to an `i2c_bus` handle, then use the Bosch API. I2C address is `BMM350_I2C_ADSEL_SET_LOW` (0x14) or `BMM350_I2C_ADSEL_SET_HIGH` (0x15), depending on ADSEL. Compensated magnetometer data is `float` in micro-tesla.

`common/*` holds the bus handle, device handle, and I2C address in one set of file-level variables, so it drives a single BMM350. Wiring both ADSEL addresses at once would need a second copy of this glue. Other sensors are unaffected — BMM150 and BMM350 can share a bus, because each component keeps its own state. Transfers themselves are serialized by `i2c_bus`, but `bmm350_set_i2c_address()` and `bmm350_interface_init()` replace the device handle, so do not call them while another task is talking to the sensor.

```c
struct bmm350_dev dev;
struct bmm350_mag_temp_data data;

bmm350_set_i2c_bus_handle(i2c_bus);
bmm350_set_i2c_address(BMM350_I2C_ADSEL_SET_LOW);
bmm350_interface_init(&dev);
bmm350_init(&dev);

bmm350_set_odr_performance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_4, &dev);
bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, &dev);
bmm350_set_powermode(BMM350_NORMAL_MODE, &dev);

bmm350_get_compensated_mag_xyz_temp_data(&data, &dev);
```

See [bmm350.h](./bmm350.h) and [common/bmm350_common.h](./common/bmm350_common.h) for the full API.

---
