#include "wifi_board.h"
#include "audio_codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "axp173.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "esp32_camera.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "assets/lang_config.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "power_save_timer.h"


#define AXP173_IRQ_GPIO 3

#include "freertos/queue.h"
#include "driver/gpio.h"

// 全局pmic指针定义，供功放/电源等全局控制使用
Axp173* g_pmic_ptr = nullptr;

// 前向声明
class JiangLianS3CamBoard;

// 全局事件队列
static QueueHandle_t axp173_evt_queue = nullptr;

// ISR回调
static void IRAM_ATTR axp173_irq_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(axp173_evt_queue, &gpio_num, NULL);
}

// 任务处理函数声明（实现放到类定义后）
static void axp173_irq_task(void* arg);

#define TAG "JiangLianS3CamBoard"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Pmic : public Axp173
{
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp173(i2c_bus, addr)
    {
        WriteReg(0x12, 0b00000111);        // REG 12H: 电源输出控制 7保留/6EXTEN关闭/5保留/4 DC-DC2关闭/3 LDO3关闭/2 LDO2开启/1 LDO4开启/0 DC-DC1开启
        WriteReg(0x26, (3300 - 700) / 25); // REG 26H: DC-DC1输出电压设置 7-6保留/6-0 Bit6-Bit0 3.3V
        WriteReg(0x27, (3300 - 700) / 25); // REG 27H: LDO4输出电压设置  7-6保留/6-0 Bit6-Bit0 3.3V
        WriteReg(0x28, 0b10101010);        // REG 28H: LDO23输出电压设置2.8V
        WriteReg(0x33, 0b11000100);        // REG 33H: 充电控制1 4.2V 450mA
        WriteReg(0x36, 0b01101100);        // REG 36H: PEK按键参数设置短按开机，长按4s关机
        //WriteReg(0x10, 0b00000000);        // REG 10H: EXTEN 做为音频使能控制，初始时关闭
        //写一遍默认值避免遇到定制芯片
        WriteReg(0x30, 0b01001000);        // REG 30H: VBUS-IPSOUT通路管理
        WriteReg(0x31, 0b00000001);        // REG 31H: VOFF关机电压设置2.7V
        WriteReg(0x32, 0b01000000);        // REG 32H: 关机设置、电池检测以及CHGLED管脚控制
        WriteReg(0x3A, 0x68);              // REG 3AH: APS 低电级别1
        WriteReg(0x3B, 0x5F);              // REG 3AH: APS 低电级别2
        WriteReg(0x84, 0b00110100);        // REG 84H: ADC采样速率设置，TS管脚控制 
        WriteReg(0x8A, 0b00000000);        // REG 8AH: 定时器控制  
        WriteReg(0x8B, 0b00000000);        // REG 8BH: VBUS管脚监测SRP功能控制
        WriteReg(0x8F, 0b00000000);        // REG 8FH: 过温关机等功能设置
        WriteReg(0x40, 0b11011110);        // REG 40H: IRQ使能1
        WriteReg(0x41, 0b11111111);        // REG 41H: IRQ使能2
        WriteReg(0x42, 0b10111011);        // REG 42H: IRQ使能3
        WriteReg(0x43, 0b11110011);        // REG 42H: IRQ使能4
        ESP_LOGI(TAG, "AXP173电源配置完成");
    }
    
    // 新增：公开读取reg46的方法
    uint8_t ReadIrqReg46() { return Axp173::ReadReg(0x46); }
};

class CustomAudioCodec : public BoxAudioCodec {
public:
    CustomAudioCodec(void* i2c_master_handle)
        : BoxAudioCodec(i2c_master_handle,
                        AUDIO_INPUT_SAMPLE_RATE,
                        AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK,
                        AUDIO_I2S_GPIO_BCLK,
                        AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT,
                        AUDIO_I2S_GPIO_DIN,
                        GPIO_NUM_NC, // 这里传NC，实际功放由AXP173 EXTEN控制
                        AUDIO_CODEC_ES8311_ADDR,
                        AUDIO_CODEC_ES7210_ADDR,
                        AUDIO_INPUT_REFERENCE) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        // 直接控制AXP173 EXTEN
        if (g_pmic_ptr) {
            g_pmic_ptr->SetExten(enable ? 1 : 0);
        }
    }
};

class JiangLianS3CamBoard : public WifiBoard
{
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    LcdDisplay *display_;
    Esp32Camera* camera_;

    Pmic *pmic_ = nullptr;
    PowerSaveTimer *power_save_timer_;
    // 电池供电时定时自动关机，节省电量
    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300); // CPU频率，60秒后进入休眠，300秒后关机 -1为禁用
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("sleepy");
            GetBacklight()->SetBrightness(10); }); // 休眠后的屏幕亮度
        power_save_timer_->OnExitSleepMode([this]() {
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); }); // 关机请求
        power_save_timer_->SetEnabled(true);
    }
    // I2C初始化
    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp173()
    {
        ESP_LOGI(TAG, "Init AXP173");
        pmic_ = new Pmic(i2c_bus_, 0x34);
        g_pmic_ptr = pmic_;
        // 新增：打印AXP173中断状态寄存器
        pmic_->PrintIrqStatusRegs();
    }
    // SPI初始化（用于显示屏）
    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }
    // 按钮初始化
    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState(); });
        // 右按钮音量+
        volume_up_button_.OnClick([this]() {
                power_save_timer_->WakeUp();
                auto codec = GetAudioCodec();
                auto volume = codec->output_volume() + 10;
                if (volume > 100) {
                    volume = 100;
                }
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        // 长按右按钮静音
        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED); });
        // 音量-，由AXP173中断处理
    }
    // 显示屏初始化
    void InitializeSt7789Display()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        // 创建显示屏对象
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_20_4,
                                         .icon_font = &font_awesome_20_4,

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                         .emoji_font = font_emoji_32_init(),
#else
                                         .emoji_font = font_emoji_64_init(),
#endif
                                     });
    }

    void InitializeCamera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_2;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
    config.ledc_timer = LEDC_TIMER_2; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
    config.pin_sccb_scl = CAMERA_PIN_SIOC;
    config.sccb_i2c_port = 1;
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config); // 测试相机是否存在
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Camera is not plugged in or not supported, error: %s", esp_err_to_name(err));
        camera_ = nullptr;
        return;
    } else {
        esp_camera_deinit();    // 释放之前的摄像头资源,为正确初始化做准备
        camera_ = new Esp32Camera(config);
    }
}

public:
    // 构造函数
    JiangLianS3CamBoard() : boot_button_(BOOT_BUTTON_GPIO),
                         touch_button_(TOUCH_BUTTON_GPIO),
                         volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                         volume_down_button_(VOLUME_DOWN_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeAxp173();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeCamera();
        //InitializeIot();
        InitializePowerSaveTimer();
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto &thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Battery"));
        //thing_manager.AddThing(iot::CreateThing("Shutdown"));
        // thing_manager.AddThing(iot::CreateThing("Lamp"));
        // 可以添加更多IoT设备
#endif
        GetBacklight()->RestoreBrightness();
        // 新增：初始化AXP173 IRQ GPIO
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_NEGEDGE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << AXP173_IRQ_GPIO);
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&io_conf);

        axp173_evt_queue = xQueueCreate(2, sizeof(uint32_t));
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)AXP173_IRQ_GPIO, axp173_irq_isr_handler, (void*)AXP173_IRQ_GPIO);

        // 创建中断处理任务
        xTaskCreate(axp173_irq_task, "axp173_irq_task", 4096, this, 10, NULL);
    }

    virtual Led *GetLed() override
    {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static CustomAudioCodec audio_codec(i2c_bus_);
        return &audio_codec;
    }

    // 获取显示屏
    virtual Display *GetDisplay() override
    {
        return display_;
    }
    // 获取背光控制
    virtual Backlight *GetBacklight() override
    {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    // 刷新电池电量信息与休眠控制
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled)
        {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
    // 获取pmic指针
    Pmic* GetPmic() { return pmic_; }
};

// 任务处理函数实现（放到完整类定义后）
static void axp173_irq_task(void* arg) {
    JiangLianS3CamBoard* board = (JiangLianS3CamBoard*)arg;
    uint32_t io_num;
    while (true) {
        if (xQueueReceive(axp173_evt_queue, &io_num, portMAX_DELAY)) {
            //ESP_LOGI(TAG, "AXP173 IRQ GPIO中断触发，读取寄存器...");
            if (board && board->GetPmic()) {
                // 读取IRQ寄存器（通过新增的public方法）
                uint8_t reg46 = board->GetPmic()->ReadIrqReg46();
                // IRQ22: PEK短按
                if (reg46 & (1 << 1)) {
                    //board->GetPowerSaveTimer()->WakeUp();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    // 音量-，由AXP173中断处理
                    auto codec = board->GetAudioCodec();
                    auto volume = codec->output_volume() - 10;
                    if (volume < 0) {
                        volume = 0;
                    }
                    codec->SetOutputVolume(volume);
                    board->GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
                }
                // 最后再清除中断标志
                board->GetPmic()->PrintIrqStatusRegs();
            }
        }
    }
}
DECLARE_BOARD(JiangLianS3CamBoard);