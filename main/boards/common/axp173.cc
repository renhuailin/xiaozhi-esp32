#include "axp173.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>
#include <algorithm>
#include "system_info.h"
#define TAG "Axp173"

Axp173::Axp173(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
}
//控制EXTEN_AU_EN
void Axp173::SetExten(int on) {
    // 读取当前寄存器值
    uint8_t reg = ReadReg(AXP173_REG_10_EXTEN);
    if (on)
        reg |= (1 << 2); // EXTEN置1
    else
        reg &= ~(1 << 2); // EXTEN清0
    // 写回寄存器
    WriteReg(AXP173_REG_10_EXTEN, reg);
}

// 检测是否正在充电
bool Axp173::IsCharging() {
    uint8_t value = ReadReg(AXP173_REG_01_CHARGE_STATUS);
    return (value & CHARGING_BIT);
}

// 检测充电是否完成
bool Axp173::IsChargingDone() {
    uint8_t value = ReadReg(AXP173_REG_01_CHARGE_STATUS);
    // 充电完成条件：不在充电状态且有电池连接
    return !(value & CHARGING_BIT) && (value & BAT_PRESENT_BIT);
}

// 判断电池放电状态
bool Axp173::IsDischarging() {
    uint8_t reg00 = ReadReg(AXP173_REG_00_INPUT_STATUS);
    uint8_t reg01 = ReadReg(AXP173_REG_01_CHARGE_STATUS);
    
    // 放电条件：
    // 1. 电流方向为放电 (REG00 bit2=0)
    // 2. 没有有效外部电源
    // 3. 电池存在且未在充电
    bool bat_connected = (reg01 & BAT_PRESENT_BIT);
    bool no_external_power = !(reg00 & (VBUS_USABLE_BIT | ACIN_USABLE_BIT));
    bool current_direction = !(reg00 & BAT_CURRENT_DIR_BIT);
    
    return bat_connected && no_external_power && current_direction;
}

//返回高八位 + 低四位电池电压   地址：高0x78 低0x79 精度：1.1mV
float Axp173::getBatVoltage() {
    float ADCLSB = 1.1 / 1000.0;
    uint8_t high = ReadReg(0x78);
    uint8_t low = ReadReg(0x79);
    float voltage = (high << 4 | (low & 0x0F)) * ADCLSB; // 合并高低位
    //ESP_LOGI(TAG, "Battery Voltage: %.3fV (Raw: 0x%02X%02X)", voltage, high, low);
    return voltage;
}

//返回电池电量等级（%）
int Axp173::GetBatteryLevel() {
    const float batVoltage = getBatVoltage();
    const float batPercentage = (batVoltage < 3.248088) ? 0 : (batVoltage - 3.120712) * 100;
    const int percentage = static_cast<int>(batPercentage);
    int result = (percentage > 100) ? 100 : percentage;
    //ESP_LOGI(TAG, "Battery level: %d%%", result);
    return result;
}

//切断电源
void Axp173::PowerOff() {
    // 1. 检查I2C有效性
    if (!i2c_device_) {
        ESP_LOGE(TAG, "I2C未初始化！");
        return;
    }
    ESP_LOGI(TAG, "写入关机寄存器...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint8_t value = ReadReg(0x32);
    value = value | 0B10000000;
    WriteReg(0x32, value);
}

// 新增：检测VBUS是否插入
bool Axp173::IsVbusPresent() {
    uint8_t reg00 = ReadReg(AXP173_REG_00_INPUT_STATUS);
    bool present = (reg00 & VBUS_PRESENT_BIT) != 0;
    //ESP_LOGD(TAG, "VBUS状态检测: REG00=0x%02X, VBUS_PRESENT=%s", reg00, present ? "是" : "否");
    return present;
}

// 打印AXP173中断状态寄存器（0x44~0x47）

void Axp173::PrintIrqStatusRegs() {
    uint8_t reg44 = ReadReg(0x44);
    uint8_t reg45 = ReadReg(0x45);
    uint8_t reg46 = ReadReg(0x46);
    uint8_t reg47 = ReadReg(0x47);
    /*
    ESP_LOGI(TAG, "AXP173 IRQ Status Registers:");
    ESP_LOGI(TAG, "REG44H: 0x%02X", reg44);
    ESP_LOGI(TAG, "REG45H: 0x%02X", reg45);
    ESP_LOGI(TAG, "REG46H: 0x%02X", reg46);
    ESP_LOGI(TAG, "REG47H: 0x%02X", reg47);
    */

    // 读取寄存器后，写回以清除中断标志
    WriteReg(0x44, reg44);
    WriteReg(0x45, reg45);
    WriteReg(0x46, reg46);
    WriteReg(0x47, reg47);
}