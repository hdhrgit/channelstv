// Board bring-up, see board.h. The 1.54's pins are from its factory firmware's config, the
// AMOLED-1.8's from Waveshare's board support package for it (both versions).

#include "board.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/temperature_sensor.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#define PIN_I2C_SDA    GPIO_NUM_42
#define PIN_I2C_SCL    GPIO_NUM_41
#define PIN_I2S_MCLK   GPIO_NUM_8
#define PIN_I2S_BCLK   GPIO_NUM_9
#define PIN_I2S_WS     GPIO_NUM_10
#define PIN_I2S_DOUT   GPIO_NUM_12
#define PIN_PA         GPIO_NUM_7
#define PIN_LCD_CS     GPIO_NUM_21
#define PIN_LCD_MOSI   GPIO_NUM_39
#define PIN_LCD_CLK    GPIO_NUM_38
#define PIN_LCD_DC     GPIO_NUM_45
#define PIN_LCD_RST    GPIO_NUM_40
#define PIN_LCD_BL     GPIO_NUM_46
#define LCD_PCLK_HZ    (40 * 1000 * 1000)
#define PIN_TP_INT     GPIO_NUM_48
#define PIN_TP_RST     GPIO_NUM_47
#define PIN_KEY_POWER  GPIO_NUM_5
#define PIN_KEY_UP     GPIO_NUM_4
#define PIN_KEY_DOWN   GPIO_NUM_0
#define PIN_BAT_EN     GPIO_NUM_2  // high keeps the board on when running from the battery
#define PIN_BAT_ADC    GPIO_NUM_1  // ADC1 channel 0, battery through a 1/3 divider
#define PIN_CHARGING   GPIO_NUM_3  // low while the charger is charging

// AMOLED-1.8
#define A_I2C_SDA      GPIO_NUM_15
#define A_I2C_SCL      GPIO_NUM_14
#define A_I2S_MCLK     GPIO_NUM_16
#define A_I2S_BCLK     GPIO_NUM_9
#define A_I2S_WS       GPIO_NUM_45
#define A_I2S_DOUT     GPIO_NUM_8
#define A_PA           GPIO_NUM_46
#define A_LCD_CS       GPIO_NUM_12
#define A_LCD_CLK      GPIO_NUM_11
#define A_LCD_D0       GPIO_NUM_4
#define A_LCD_D1       GPIO_NUM_5
#define A_LCD_D2       GPIO_NUM_6
#define A_LCD_D3       GPIO_NUM_7
#define A_LCD_PCLK_HZ  (40 * 1000 * 1000)
#define A_TP_INT       GPIO_NUM_21
#define A_KEY_BOOT     GPIO_NUM_0
#define A_GAP_V2       16 // the CO5300's picture starts 16 columns in
#define EXP_ADDR       0x20 // TCA9554 (V1) / TCAL9534 (V2): panel and touch power and resets
#define PMU_ADDR       0x34 // AXP2101
#define RTC_ADDR       0x51 // PCF85063

#define IMU_ADDR  0x6b // QMI8658 (0x6a with its address pin low)

#define LONG_MS   3000 // holding PWR this long powers off
#define HOLD_MS   500  // from here on a PWR press is a hold, not a play/stop tap
#define REPEAT_MS 250

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static esp_codec_dev_handle_t s_spk;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static temperature_sensor_handle_t s_temp;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static char s_i2c_devs[64];
static i2c_master_dev_handle_t s_imu, s_pmu;
static board_info_t s_info = {BOARD_LCD154, "Waveshare ESP32-S3-Touch-LCD-1.54", "", "ST7789 240x240 SPI", "CST816 (0x15)"};
int g_lcd_w = 240, g_lcd_h = 240;
static uint8_t s_imu_addr, s_exp_in;
static int s_gpio_pa = PIN_PA;

#define AMOLED (s_info.id == BOARD_AMOLED18)

const board_info_t *board_info(void)
{
    return &s_info;
}

static esp_err_t reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *v, size_t n)
{
    return d ? i2c_master_transmit_receive(d, &reg, 1, v, n, 50) : ESP_ERR_INVALID_STATE;
}

static esp_err_t reg_write(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return d ? i2c_master_transmit(d, b, 2, 50) : ESP_ERR_INVALID_STATE;
}

static i2c_master_dev_handle_t dev_add(uint8_t addr)
{
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t h = NULL;
    return i2c_master_bus_add_device(s_i2c, &dev, &h) == ESP_OK ? h : NULL;
}

// The AMOLED-1.8 has its I2C bus on GPIO 14/15, which the 1.54 leaves unconnected; with the
// pull-ups on, a probe there finds nothing on the 1.54 and drives none of its pins. If the
// AXP2101 answers, the bus stays up for i2c_init().
static bool find_amoled(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = A_I2C_SDA,
        .scl_io_num = A_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (i2c_new_master_bus(&cfg, &s_i2c) != ESP_OK) {
        return false;
    }
    if (i2c_master_probe(s_i2c, PMU_ADDR, 50) == ESP_OK || i2c_master_probe(s_i2c, EXP_ADDR, 50) == ESP_OK) {
        return true;
    }
    i2c_del_master_bus(s_i2c);
    s_i2c = NULL;
    gpio_reset_pin(A_I2C_SDA);
    gpio_reset_pin(A_I2C_SCL);
    return false;
}

// Panel power on and the panel's and touch chip's resets let go, through the IO expander,
// as Waveshare's bring-up does: SD CS high, resets low, then everything high.
static void amoled_expander(void)
{
    i2c_master_dev_handle_t e = dev_add(EXP_ADDR);
    reg_write(e, 0x03, (uint8_t)~(0x01 | 0x02 | 0x04 | 0x80)); // P0 panel reset, P1 panel power, P2 touch reset, P7 SD CS
    reg_write(e, 0x01, 0x80);
    vTaskDelay(pdMS_TO_TICKS(20));
    reg_write(e, 0x01, 0x01 | 0x02 | 0x04 | 0x80);
    vTaskDelay(pdMS_TO_TICKS(150));
    reg_read(e, 0x00, &s_exp_in, 1);
    if (e) {
        i2c_master_bus_rm_device(e);
    }
}

// The AXP2101 keeps its own rails as the factory set them; this turns on its battery and
// USB measurements (the TS pin has no thermistor on this board) and the PWR key's events.
static void amoled_pmu(void)
{
    s_pmu = dev_add(PMU_ADDR);
    uint8_t v = 0;
    if (reg_read(s_pmu, 0x03, &v, 1) != ESP_OK || v != 0x4a) {
        ESP_LOGW(TAG, "no AXP2101 (id 0x%02x)", v);
        return;
    }
    if (reg_read(s_pmu, 0x30, &v, 1) == ESP_OK) {
        reg_write(s_pmu, 0x30, (v | 0x01 | 0x04) & ~0x02); // battery and VBUS voltage, not TS
    }
    if (reg_read(s_pmu, 0x68, &v, 1) == ESP_OK) {
        reg_write(s_pmu, 0x68, v | 0x01); // battery detection
    }
    reg_write(s_pmu, 0x41, 0x03); // INTEN2: PWR key pressed and let go
    reg_write(s_pmu, 0x48, 0xff);
    reg_write(s_pmu, 0x49, 0xff);
    reg_write(s_pmu, 0x4a, 0xff);
}

static void power_init(void)
{
    gpio_deep_sleep_hold_dis(); // after a deep sleep: take the pins back
    if (AMOLED) {
        gpio_config_t out = {.pin_bit_mask = 1ULL << A_PA, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&out);
        gpio_set_level(A_PA, 0);
    } else {
        gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_BAT_EN | 1ULL << PIN_PA, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&out);
        gpio_hold_dis(PIN_BAT_EN);
        gpio_set_level(PIN_BAT_EN, 1);
        gpio_set_level(PIN_PA, 0);
        gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_CHARGING, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
        gpio_config(&in);
    }

    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_temp) == ESP_OK) {
        temperature_sensor_enable(s_temp);
    } else {
        s_temp = NULL;
    }
    if (AMOLED) {
        return; // the battery is the AXP2101's
    }

    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&unit, &s_adc);
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan);
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
}

static void i2c_init(void)
{
    if (!s_i2c) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = 1,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
    }
    int pos = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c, addr, 20) == ESP_OK) {
            pos += snprintf(s_i2c_devs + pos, sizeof(s_i2c_devs) - pos, "%s0x%02x", pos ? "," : "", addr);
        }
    }
    ESP_LOGI(TAG, "I2C devices: %s", s_i2c_devs);

    if (AMOLED) {
        amoled_pmu();
    }

    uint8_t who = 0;
    s_imu_addr = i2c_master_probe(s_i2c, IMU_ADDR, 20) == ESP_OK ? IMU_ADDR : IMU_ADDR - 1;
    if ((s_imu = dev_add(s_imu_addr)) && reg_read(s_imu, 0x00, &who, 1) == ESP_OK && who == 0x05) {
        if (AMOLED) {
            // left running by the factory firmware, it kept one reading until reset
            reg_write(s_imu, 0x60, 0xb0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // the temperature only updates while a sensor runs: accelerometer at its slowest rate
        uint8_t ctrl1[] = {0x02, 0x40}, ctrl2[] = {0x03, 0x0f}, ctrl7[] = {0x08, 0x01};
        i2c_master_transmit(s_imu, ctrl1, 2, 50);
        i2c_master_transmit(s_imu, ctrl2, 2, 50);
        i2c_master_transmit(s_imu, ctrl7, 2, 50);
    } else {
        ESP_LOGW(TAG, "no QMI8658 (who=0x%02x)", who);
        if (s_imu) {
            i2c_master_bus_rm_device(s_imu);
        }
        s_imu = NULL;
        s_imu_addr = 0;
    }
}

// Speaker only: the 1.54's ES7210 mics share the I2S port, and the AMOLED-1.8's mic is on the
// ES8311 itself, but nothing here uses them.
static void audio_init(void)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    chan_cfg.auto_clear_after_cb = true; // silence when nothing is playing
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AMOLED ? A_I2S_MCLK : PIN_I2S_MCLK,
            .bclk = AMOLED ? A_I2S_BCLK : PIN_I2S_BCLK,
            .ws = AMOLED ? A_I2S_WS : PIN_I2S_WS,
            .dout = AMOLED ? A_I2S_DOUT : PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, // board_amp() switches it with the player
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    s_spk = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = SAMPLE_RATE};
    if (!s_spk || esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker codec failed");
        s_spk = NULL;
    }
}

static void backlight_init(void)
{
    if (AMOLED) {
        return; // an AMOLED has no backlight: its brightness is a panel command
    }
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&ch);
}

// ---------------------------------------------------------------- touch calibration

// Raw points from the CST816 become screen points through the calibration: an axis swap, then
// screen = a * raw + b on each axis. The last points seen are kept, raw and mapped, for the
// web API.
static touch_cal_t s_cal = {.ax = 1, .ay = 1};
static volatile int s_rot, s_rot_want; // the panel's rotation (board_rotation_apply)
static touch_log_t s_tlog[TOUCH_LOG_N];
static int s_tlog_n;
static bool s_tdown;
static int s_traw_x = -1, s_traw_y = -1;
static portMUX_TYPE s_tmux = portMUX_INITIALIZER_UNLOCKED;

static void touch_map(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num,
                      uint8_t max_point_num)
{
    bool down = *point_num > 0;
    if (down) {
        int rx = x[0], ry = y[0];
        float u = s_cal.swap ? ry : rx, v = s_cal.swap ? rx : ry;
        int mx = (int)(s_cal.ax * u + s_cal.bx + 0.5f), my = (int)(s_cal.ay * v + s_cal.by + 0.5f);
        mx = mx < 0 ? 0 : mx >= LCD_W ? LCD_W - 1 : mx;
        my = my < 0 ? 0 : my >= LCD_H ? LCD_H - 1 : my;
        // the calibration is the panel's own way up: turn the point as the picture is turned
        int px = mx;
        switch (s_rot) {
        case 1: mx = LCD_H - 1 - my; my = px; break;
        case 2: mx = LCD_W - 1 - mx; my = LCD_H - 1 - my; break;
        case 3: mx = my; my = LCD_W - 1 - px; break;
        default: break;
        }
        x[0] = mx;
        y[0] = my;
        portENTER_CRITICAL(&s_tmux);
        if (rx != s_traw_x || ry != s_traw_y || !s_tdown) {
            touch_log_t *l = &s_tlog[s_tlog_n++ % TOUCH_LOG_N];
            *l = (touch_log_t){.ms = esp_timer_get_time() / 1000, .raw_x = rx, .raw_y = ry, .x = mx, .y = my, .down = true};
        }
        s_traw_x = rx;
        s_traw_y = ry;
        portEXIT_CRITICAL(&s_tmux);
    } else if (s_tdown) {
        portENTER_CRITICAL(&s_tmux);
        touch_log_t *l = &s_tlog[s_tlog_n++ % TOUCH_LOG_N];
        *l = (touch_log_t){.ms = esp_timer_get_time() / 1000, .raw_x = s_traw_x, .raw_y = s_traw_y, .x = -1, .y = -1};
        portEXIT_CRITICAL(&s_tmux);
    }
    s_tdown = down;
}

void board_touch_cal(touch_cal_t *out)
{
    *out = s_cal;
}

void board_touch_set_cal(const touch_cal_t *cal)
{
    s_cal = *cal;
}

bool board_touch_raw(int *x, int *y)
{
    *x = s_traw_x;
    *y = s_traw_y;
    return s_traw_x >= 0;
}

int board_touch_log(touch_log_t *out, int max)
{
    portENTER_CRITICAL(&s_tmux);
    int n = s_tlog_n < TOUCH_LOG_N ? s_tlog_n : TOUCH_LOG_N, k = 0;
    for (int i = s_tlog_n - n; i < s_tlog_n && k < max; i++) {
        out[k++] = s_tlog[i % TOUCH_LOG_N];
    }
    portEXIT_CRITICAL(&s_tmux);
    return k;
}

// Waveshare's init sequence for both panels (the SH8601 takes the CO5300's commands): 16-bit
// colour, tearing effect output on, brightness up, the 368x448 window.
static const co5300_lcd_init_cmd_t s_amoled_init[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static uint32_t s_panel_id; // RDDID's three bytes, 0 if it couldn't be read

static void amoled_lcd_init(bool v2)
{
    spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(A_LCD_CLK, A_LCD_D0, A_LCD_D1, A_LCD_D2, A_LCD_D3,
                                                        LCD_W * 40 * 2);
    bus.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1; // with the video's draw task, as on the 1.54
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(A_LCD_CS, NULL, NULL);
    io.pclk_hz = A_LCD_PCLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &s_lcd_io));
    co5300_vendor_config_t vendor = {
        .init_cmds = s_amoled_init,
        .init_cmds_size = sizeof(s_amoled_init) / sizeof(s_amoled_init[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, // the expander did it
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_lcd_io, &panel, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, v2 ? A_GAP_V2 : 0, 0);
    // RDDID, read back on one line
    uint8_t id[3] = {0};
    if (esp_lcd_panel_io_rx_param(s_lcd_io, 0x03 << 24 | 0x04 << 8, id, 3) == ESP_OK) {
        s_panel_id = id[0] << 16 | id[1] << 8 | id[2];
    }
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = v2 ? (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()
                                                 : (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io));
    esp_lcd_touch_config_t tp = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = -1, // the expander did it
        .int_gpio_num = A_TP_INT,
        .process_coordinates = touch_map,
    };
    esp_err_t err = v2 ? esp_lcd_touch_new_i2c_cst816s(tp_io, &tp, &s_touch) : esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch failed");
        s_touch = NULL;
    }
}

static void lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,
        // the interrupt on core 1 with the video's draw task: with it on core 0 the draw task
        // (core 1) hung for good in spi_device_acquire_bus during live TV, every few minutes
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 3,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io, &s_lcd_io));
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_lcd_io, &panel, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    // FRCTRL2: 48 Hz instead of 60, twice the video's 24 fps (with the default porch)
    esp_lcd_panel_io_tx_param(s_lcd_io, 0xc6, (uint8_t[]){0x17}, 1);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io));
    esp_lcd_touch_config_t tp = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .process_coordinates = touch_map,
    };
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch failed");
        s_touch = NULL;
    }
}

// The microphone ADC isn't used, but at power-on it runs with its mic amplifiers on. The
// register writes its driver makes on stop (mics, analog part and clocks off) put it to sleep.
static void mics_sleep(void)
{
    if (AMOLED) {
        return; // no ES7210
    }
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x40, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t es7210;
    if (i2c_master_bus_add_device(s_i2c, &dev, &es7210) != ESP_OK) {
        return;
    }
    static const uint8_t seq[][2] = {{0x47, 0xff}, {0x48, 0xff}, {0x49, 0xff}, {0x4a, 0xff}, {0x4b, 0xff},
                                     {0x4c, 0xff}, {0x40, 0xc0}, {0x01, 0x7f}, {0x06, 0x07}};
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        i2c_master_transmit(es7210, seq[i], 2, 50);
    }
    i2c_master_bus_rm_device(es7210);
}

void board_init(void)
{
    if (find_amoled()) {
        // V2 swapped the panel and the touch chip: its CST816 answers at 0x15, V1's FT3168 at 0x38
        amoled_expander();
        bool v2 = i2c_master_probe(s_i2c, 0x15, 50) == ESP_OK;
        s_info = (board_info_t){BOARD_AMOLED18, "Waveshare ESP32-S3-Touch-AMOLED-1.8", v2 ? "V2" : "V1",
                                v2 ? "CO5300 368x448 QSPI" : "SH8601 368x448 QSPI", v2 ? "CST816 (0x15)" : "FT3168 (0x38)"};
        g_lcd_w = 368;
        g_lcd_h = 448;
        s_gpio_pa = A_PA;
    }
    ESP_LOGI(TAG, "%s %s", s_info.name, s_info.rev);
    power_init();
    i2c_init();
    mics_sleep();
    backlight_init();
    if (AMOLED) {
        amoled_lcd_init(!strcmp(s_info.rev, "V2"));
    } else {
        lcd_init();
    }
    audio_init();
}

esp_codec_dev_handle_t board_speaker(void)
{
    return s_spk;
}

void board_amp(bool on)
{
    gpio_set_level(s_gpio_pa, on);
}

esp_lcd_panel_io_handle_t board_lcd_io(void)
{
    return s_lcd_io;
}

esp_lcd_panel_handle_t board_lcd_panel(void)
{
    return s_panel;
}

esp_lcd_touch_handle_t board_touch(void)
{
    return s_touch;
}

static int s_backlight;

int board_backlight_percent(void)
{
    return s_backlight;
}

void board_backlight(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_backlight = percent;
    if (AMOLED) {
        if (s_panel) {
            esp_lcd_panel_co5300_set_brightness(s_panel, percent);
        }
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---------------------------------------------------------------- keys

static key_fn s_key_fn;
static volatile int s_held_ms;

// The AMOLED-1.8's PWR key (button 2) belongs to the AXP2101, which tells when it goes down
// and when it comes up again: it does all the 1.54's PWR does, and switches the board on.
// BOOT (button 1): a tap goes back as PWR's does, held it turns the volume down a step at a
// time, round from 0 to 100 again.
static bool s_pwr_down;

// Whether PWR is down now; *tap if it went down and up again since the last look.
static bool pmu_key(bool *tap)
{
    uint8_t st = 0;
    *tap = false;
    if (reg_read(s_pmu, 0x49, &st, 1) == ESP_OK && (st & 0x03)) {
        reg_write(s_pmu, 0x49, st & 0x03);
        if ((st & 0x03) == 0x03 && !s_pwr_down) {
            *tap = true; // both within one look: a quick tap
        } else {
            s_pwr_down = st & 0x02 && !(st & 0x01);
        }
    }
    return s_pwr_down;
}

static void keys_task(void *arg)
{
    gpio_num_t pins[3] = {PIN_KEY_POWER, PIN_KEY_UP, PIN_KEY_DOWN};
    int nkeys = 3;
    if (AMOLED) {
        pins[0] = GPIO_NUM_NC; // the AXP2101's
        pins[1] = A_KEY_BOOT;
        nkeys = 2;
    }
    int held_ms[3] = {0};
    bool long_sent[3] = {0};
    // a key held at boot (BOOT is GPIO0) must be released before it counts
    bool armed[3];
    for (int i = 0; i < nkeys; i++) {
        armed[i] = pins[i] == GPIO_NUM_NC || gpio_get_level(pins[i]) == 1;
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool pwr_down = false, pwr_tap = false;
        if (AMOLED) {
            pwr_down = pmu_key(&pwr_tap);
            if (pwr_tap) {
                s_key_fn(KEY_POWER, PRESS_SHORT);
            }
        }
        for (int i = 0; i < nkeys; i++) {
            bool down = pins[i] == GPIO_NUM_NC ? pwr_down : gpio_get_level(pins[i]) == 0;
            if (!armed[i]) {
                armed[i] = !down;
                continue;
            }
            if (down) {
                held_ms[i] += 20;
                if (i == KEY_POWER && held_ms[i] >= LONG_MS && !long_sent[i]) {
                    long_sent[i] = true;
                    s_held_ms = LONG_MS;
                    s_key_fn(i, PRESS_LONG);
                } else if (i == KEY_POWER && held_ms[i] >= HOLD_MS && !long_sent[i] && held_ms[i] % 100 == 0) {
                    s_held_ms = held_ms[i];
                    s_key_fn(i, PRESS_HOLD); // ten a second, for the power-off countdown
                } else if (i != KEY_POWER && held_ms[i] >= 500 && held_ms[i] % REPEAT_MS == 0) {
                    long_sent[i] = true;
                    s_key_fn(AMOLED ? KEY_VOL_CYCLE : i, PRESS_REPEAT); // volume keys repeat while held
                }
            } else {
                if (i == KEY_POWER && held_ms[i] >= HOLD_MS && !long_sent[i]) {
                    s_key_fn(i, PRESS_CANCEL); // let go before the power-off
                } else if (held_ms[i] >= 40 && !long_sent[i]) {
                    s_key_fn(AMOLED && i == 1 ? KEY_POWER : i, PRESS_SHORT);
                }
                held_ms[i] = 0;
                long_sent[i] = false;
            }
        }
    }
}

int board_power_hold_ms(void)
{
    return s_held_ms;
}

void board_keys_start(key_fn fn)
{
    s_key_fn = fn;
    gpio_config_t in = {
        .pin_bit_mask = AMOLED ? 1ULL << A_KEY_BOOT : 1ULL << PIN_KEY_POWER | 1ULL << PIN_KEY_UP | 1ULL << PIN_KEY_DOWN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&in);
    xTaskCreate(keys_task, "keys", 3072, NULL, 5, NULL);
}

// ---------------------------------------------------------------- stats

// Battery voltage (under the board's own ~WiFi load) to charge, in 10 % steps from 0 to
// 100 %. Measured 2026-10-31: it cut out at 2.90 V; a full charge from empty took 41 min
// and, off the charger, settled at ~4.02 V. 0 % sits above the cut-off for a few minutes'
// warning.
static const int s_curve_mv[11] = {3050, 3500, 3640, 3700, 3740, 3780, 3820, 3860, 3900, 3950, 4000};
static const int s_curve_pct[11] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
// On the charger the voltage reads high: the same battery's voltage while charging, against
// how much of that 41-minute charge had passed (4.00 V is about half full there).
static const int s_charge_mv[11] = {3300, 3680, 3820, 3910, 3955, 4000, 4030, 4055, 4095, 4155, 4180};
static const int s_charge_pct[11] = {0, 8, 19, 29, 39, 50, 60, 69, 79, 91, 99};
static int s_charging_pct = -1; // the level shown while charging, which only goes up

static int battery_pct(int mv, const int *mvs, const int *pcts)
{
    if (mv <= mvs[0]) {
        return pcts[0];
    }
    for (int i = 0; i < 10; i++) {
        if (mv < mvs[i + 1]) {
            return pcts[i] + (pcts[i + 1] - pcts[i]) * (mv - mvs[i]) / (mvs[i + 1] - mvs[i]);
        }
    }
    return pcts[10];
}

// The AXP2101's own fuel gauge, voltage and charger state
static void pmu_battery(battery_t *out)
{
    uint8_t st[2] = {0}, v[2] = {0}, pct = 0;
    *out = (battery_t){0};
    if (reg_read(s_pmu, 0x00, st, 2) != ESP_OK) {
        return;
    }
    out->usb = (st[0] & 0x20) || usb_serial_jtag_is_connected(); // VBUS good
    out->charging = (st[1] >> 5) == 1;
    if (!(st[0] & 0x08)) {
        return; // no battery
    }
    if (reg_read(s_pmu, 0x34, v, 2) == ESP_OK) {
        out->mv = (v[0] & 0x1f) << 8 | v[1];
    }
    if (reg_read(s_pmu, 0xa4, &pct, 1) == ESP_OK) {
        out->percent = pct > 100 ? 100 : pct;
    }
    if (out->charging && out->percent > 99) {
        out->percent = 99;
    }
}

void board_battery(battery_t *out)
{
    if (AMOLED) {
        pmu_battery(out);
        return;
    }
    out->charging = gpio_get_level(PIN_CHARGING) == 0;
    out->usb = out->charging || usb_serial_jtag_is_connected();
    out->mv = 0;
    out->percent = 0;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
            continue;
        }
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
        } else {
            sum += raw * 3100 / 4095;
        }
        n++;
    }
    if (!n) {
        return;
    }
    out->mv = sum / n * 3;
    if (out->charging) {
        int pct = battery_pct(out->mv, s_charge_mv, s_charge_pct);
        s_charging_pct = pct > s_charging_pct ? pct : s_charging_pct;
        out->percent = s_charging_pct;
    } else {
        s_charging_pct = -1;
        out->percent = battery_pct(out->mv, s_curve_mv, s_curve_pct);
    }
}

float board_chip_temp(void)
{
    float t = 0;
    if (!s_temp || temperature_sensor_get_celsius(s_temp, &t) != ESP_OK) {
        return -273;
    }
    return t;
}

bool board_accel(float *x, float *y, float *z)
{
    uint8_t reg = 0x35, a[6];
    if (!s_imu || i2c_master_transmit_receive(s_imu, &reg, 1, a, 6, 50) != ESP_OK) {
        return false;
    }
    *x = (int16_t)(a[0] | a[1] << 8) / 16384.0f; // +-2 g
    *y = (int16_t)(a[2] | a[3] << 8) / 16384.0f;
    *z = (int16_t)(a[4] | a[5] << 8) / 16384.0f;
    return true;
}

// ---------------------------------------------------------------- rotation

void board_rotation_want(int r)
{
    s_rot_want = r & 3;
}

int board_rotation(void)
{
    return s_rot;
}

bool board_rotation_apply(void)
{
    int r = s_rot_want;
    if (LCD_W != LCD_H) {
        r = 0; // a quarter turn changes the screen's shape, which the UI can't follow yet
    }
    if (r == s_rot || !s_panel) {
        return false;
    }
    s_rot = r;
    return true;
}

void board_turn_rect(int x, int y, int w, int h, int rot, int *px, int *py)
{
    switch (rot & 3) {
    case 1: *px = y; *py = LCD_W - x - w; break;
    case 2: *px = LCD_W - x - w; *py = LCD_H - y - h; break;
    case 3: *px = LCD_H - y - h; *py = x; break;
    default: *px = x; *py = y; break;
    }
}

void board_turn_rows(const uint16_t *src, int w, int h, int rot, int first, int lines, uint16_t *dst)
{
    board_turn_t t = board_turn(w, h, rot, first, lines);
    for (int y = t.y0; y < t.y1; y++) {
        const uint16_t *s = src + y * w;
        uint16_t *o = dst + t.k0 + t.ky * y;
        for (int x = t.x0; x < t.x1; x++) {
            o[t.kx * x] = s[x];
        }
    }
}

void board_turn_in_place(uint16_t *p, int w, int h, int rot)
{
    int n = w * h;
    rot &= 3;
    if (rot == 2) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            uint16_t t = p[i];
            p[i] = p[j];
            p[j] = t;
        }
        return;
    }
    // a quarter turn: each pixel moves along its cycle of the permutation, once
    static uint32_t done[(LCD_MAX_W * 20 + 31) / 32];
    if (rot == 0 || n > LCD_MAX_W * 20) {
        return;
    }
    memset(done, 0, (n + 31) / 32 * 4);
    for (int s0 = 0; s0 < n; s0++) {
        if (done[s0 >> 5] & 1u << (s0 & 31)) {
            continue;
        }
        int s = s0;
        uint16_t v = p[s0];
        do {
            int x = s % w, y = s / w;
            int d = rot == 1 ? (w - 1 - x) * h + y : x * h + h - 1 - y;
            uint16_t t = p[d];
            p[d] = v;
            v = t;
            done[d >> 5] |= 1u << (d & 31);
            s = d;
        } while (s != s0);
    }
}

float board_imu_temp(void)
{
    uint8_t reg = 0x33, t[2];
    if (!s_imu || i2c_master_transmit_receive(s_imu, &reg, 1, t, 2, 50) != ESP_OK) {
        return -273;
    }
    return (int16_t)(t[0] | t[1] << 8) / 256.0f;
}


bool board_panel_gamma(float gamma)
{
    if (AMOLED) {
        return false; // no built-in curves to pick from
    }
    // GAMSET picks one of the ST7789's four built-in curves
    uint8_t sel = gamma < 1.4f ? 0x08 : gamma < 2.0f ? 0x02 : gamma < 2.35f ? 0x01 : 0x04;
    return esp_lcd_panel_io_tx_param(s_lcd_io, 0x26, &sel, 1) == ESP_OK;
}


const char *board_i2c_devices(void)
{
    return s_i2c_devs;
}

void board_power_off(unsigned wake_s)
{
    board_backlight(0);
    if (AMOLED) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_disp_sleep(s_panel, true);
        if (wake_s == 0) {
            uint8_t v = 0;
            reg_read(s_pmu, 0x10, &v, 1);
            reg_write(s_pmu, 0x10, v | 0x01); // the AXP2101 cuts all power; PWR starts it again
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        // still here, or to wake by itself: deep sleep until BOOT (or the timer)
        for (int i = 0; i < 100 && gpio_get_level(A_KEY_BOOT) == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        rtc_gpio_pullup_en(A_KEY_BOOT);
        rtc_gpio_pulldown_dis(A_KEY_BOOT);
        esp_sleep_enable_ext1_wakeup_io(1ULL << A_KEY_BOOT, ESP_EXT1_WAKEUP_ANY_LOW);
        if (wake_s) {
            esp_sleep_enable_timer_wakeup((uint64_t)wake_s * 1000000ULL);
        }
        esp_deep_sleep_start();
    }
    if (wake_s == 0) {
        gpio_set_level(PIN_BAT_EN, 0);
    } // else keep the latch: only a deep sleep can wake itself up again
    // Still running: USB feeds the board, so the latch can't cut it. Sleep deeply instead and
    // start again when PWR is pressed (after it's released, or it would wake at once).
    for (int i = 0; i < 100 && gpio_get_level(PIN_KEY_POWER) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_en(PIN_KEY_POWER);
    rtc_gpio_pulldown_dis(PIN_KEY_POWER);
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_KEY_POWER, ESP_EXT1_WAKEUP_ANY_LOW);
    if (wake_s) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_s * 1000000ULL);
        gpio_hold_en(PIN_BAT_EN); // the battery latch must stay on through the sleep
        gpio_deep_sleep_hold_en();
    }
    esp_deep_sleep_start();
}

// ---------------------------------------------------------------- chips

static int addf(char *buf, size_t len, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int addf(char *buf, size_t len, int pos, const char *fmt, ...)
{
    if (pos >= (int)len) {
        return pos;
    }
    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(buf + pos, len - pos, fmt, ap);
    va_end(ap);
    return pos;
}

// Reads one register of the chip at addr; -1 if it doesn't answer.
static int peek(uint8_t addr, uint8_t reg)
{
    i2c_master_dev_handle_t d = dev_add(addr);
    uint8_t v = 0;
    esp_err_t err = reg_read(d, reg, &v, 1);
    if (d) {
        i2c_master_bus_rm_device(d);
    }
    return err == ESP_OK ? v : -1;
}

// One chip as a JSON object: its part, what it does, where it sits (addr -1: not on I2C), what
// identified it, and whether it answered.
static int chip(char *buf, size_t len, int pos, const char *part, const char *role, int addr, const char *id, bool ok)
{
    char a[8] = "";
    if (addr >= 0) {
        snprintf(a, sizeof(a), "0x%02x", addr);
    }
    return addf(buf, len, pos, "%s{\"part\":\"%s\",\"role\":\"%s\",\"addr\":\"%s\",\"id\":\"%s\",\"ok\":%s}",
                pos > 1 ? "," : "", part, role, a, id, ok ? "true" : "false");
}

void board_chips_json(char *buf, size_t len)
{
    char id[48];
    int pos = addf(buf, len, 0, "[");
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(id, sizeof(id), "rev v%d.%d, %lu MB flash, %u MB PSRAM", ci.revision / 100, ci.revision % 100,
             (unsigned long)(flash >> 20), (unsigned)(esp_psram_get_size() >> 20));
    pos = chip(buf, len, pos, "ESP32-S3", "processor, Wi-Fi", -1, id, true);
    snprintf(id, sizeof(id), "MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    pos = chip(buf, len, pos, "Wi-Fi", "station", -1, id, true);
    if (AMOLED) {
        bool v2 = !strcmp(s_info.rev, "V2");
        bool read = s_panel_id && s_panel_id != 0xffffff; // with nothing driving the line back it reads all ones
        snprintf(id, sizeof(id), read ? "RDDID %06lx" : "answers its init; its ID doesn't read back over QSPI",
                 (unsigned long)s_panel_id);
        pos = chip(buf, len, pos, v2 ? "CO5300" : "SH8601", "AMOLED driver, 368x448, QSPI", -1, id, s_panel != NULL);
        int t = peek(v2 ? 0x15 : 0x38, v2 ? 0xa7 : 0xa3);
        snprintf(id, sizeof(id), t >= 0 ? "chip ID 0x%02x" : "no answer", t);
        pos = chip(buf, len, pos, v2 ? "CST816" : "FT3168", "touch", v2 ? 0x15 : 0x38, id, t >= 0);
        int p = peek(PMU_ADDR, 0x03);
        snprintf(id, sizeof(id), p >= 0 ? "IC type 0x%02x" : "no answer", p);
        pos = chip(buf, len, pos, "AXP2101", "power, battery charger, PWR key", PMU_ADDR, id, p == 0x4a);
        int e = peek(EXP_ADDR, 0x03);
        snprintf(id, sizeof(id), e >= 0 ? "direction 0x%02x, inputs 0x%02x at boot" : "no answer", e, s_exp_in);
        pos = chip(buf, len, pos, v2 ? "TCAL9534" : "TCA9554", "IO expander: panel power and resets", EXP_ADDR, id, e >= 0);
        int r = peek(RTC_ADDR, 0x04);
        // bit 7 of the seconds: the oscillator stopped since the clock was last set
        snprintf(id, sizeof(id), r < 0 ? "no answer" : r & 0x80 ? "running, %02x s, not set" : "running, %02x s", r & 0x7f);
        pos = chip(buf, len, pos, "PCF85063", "real-time clock", RTC_ADDR, id, r >= 0);
    } else {
        pos = chip(buf, len, pos, "ST7789", "LCD driver, 240x240, SPI", -1, "", s_panel != NULL);
        int t = peek(0x15, 0xa7);
        snprintf(id, sizeof(id), t >= 0 ? "chip ID 0x%02x" : "no answer", t);
        pos = chip(buf, len, pos, "CST816", "touch", 0x15, id, t >= 0);
        int m = peek(0x40, 0xfd);
        snprintf(id, sizeof(id), m >= 0 ? "chip ID 0x%02x" : "no answer", m);
        pos = chip(buf, len, pos, "ES7210", "microphone ADC (unused)", 0x40, id, m >= 0);
    }
    int c1 = peek(0x18, 0xfd), c2 = peek(0x18, 0xfe);
    snprintf(id, sizeof(id), c1 >= 0 ? "chip ID %02x%02x" : "no answer", c1, c2);
    pos = chip(buf, len, pos, "ES8311", AMOLED ? "audio codec: speaker (and mic, unused)" : "audio codec: speaker", 0x18, id,
               c1 == 0x83);
    int w = s_imu_addr ? peek(s_imu_addr, 0x00) : -1, rv = s_imu_addr ? peek(s_imu_addr, 0x01) : -1;
    snprintf(id, sizeof(id), w >= 0 ? "WHO_AM_I 0x%02x, revision 0x%02x" : "no answer", w, rv);
    pos = chip(buf, len, pos, "QMI8658", "motion sensor", s_imu_addr ? s_imu_addr : IMU_ADDR, id, w == 0x05);
    addf(buf, len, pos, "]");
}
