#ifndef _OTTO_MPU6050_H_
#define _OTTO_MPU6050_H_

#include "i2c_device.h"

#include <cstdint>

// Minimal MPU6050 driver - just enough to read raw accelerometer samples for
// shake detection. Not a general-purpose IMU driver: no gyro, no DMP, no
// interrupt/FIFO support.
//
// Only construct this after confirming a real MPU6050 answers on the bus
// (see OttoRobot::InitializeMpu6050() in otto_robot.cc) - I2cDevice's
// WriteReg()/ReadRegs() use ESP_ERROR_CHECK internally, so touching it
// against a sensor that isn't actually wired up would panic the firmware.
class Mpu6050 : public I2cDevice {
public:
    static constexpr uint8_t kDefaultAddress = 0x68;
    static constexpr uint8_t kWhoAmIReg = 0x75;
    static constexpr uint8_t kWhoAmIValue = 0x68;

    Mpu6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr = kDefaultAddress)
        : I2cDevice(i2c_bus, addr) {
        // PWR_MGMT_1 (0x6B): the chip resets with the SLEEP bit set (0x40) -
        // clear it and leave the internal 8MHz oscillator selected (bits 2:0
        // = 0) so the accelerometer actually samples.
        WriteReg(0x6B, 0x00);
    }

    // Raw accelerometer sample, not converted to g's - shake detection only
    // needs the relative delta between consecutive samples, not absolute
    // units, so there's no need to apply the full-scale-range scale factor.
    void ReadAccel(int16_t& ax, int16_t& ay, int16_t& az) {
        uint8_t buf[6];
        ReadRegs(0x3B, buf, sizeof(buf));  // ACCEL_XOUT_H .. ACCEL_ZOUT_L
        ax = static_cast<int16_t>((buf[0] << 8) | buf[1]);
        ay = static_cast<int16_t>((buf[2] << 8) | buf[3]);
        az = static_cast<int16_t>((buf[4] << 8) | buf[5]);
    }
};

#endif  // _OTTO_MPU6050_H_
