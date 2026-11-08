// Board bring-up, see board.h. Pins from the factory firmware's config for this board.

#include "board.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#include <stdio.h>
#include <string.h>
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
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
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

#define IMU_ADDR  0x6b // QMI8658

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
static i2c_master_dev_handle_t s_imu;

static void power_init(void)
{
    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_BAT_EN | 1ULL << PIN_PA, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_deep_sleep_hold_dis(); // after a deep sleep: take the pins back
    gpio_hold_dis(PIN_BAT_EN);
    gpio_set_level(PIN_BAT_EN, 1);
    gpio_set_level(PIN_PA, 0);
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_CHARGING, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&in);

    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&unit, &s_adc);
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan);
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }

    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_temp) == ESP_OK) {
        temperature_sensor_enable(s_temp);
    } else {
        s_temp = NULL;
    }
}

static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
    int pos = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c, addr, 20) == ESP_OK) {
            pos += snprintf(s_i2c_devs + pos, sizeof(s_i2c_devs) - pos, "%s0x%02x", pos ? "," : "", addr);
        }
    }
    ESP_LOGI(TAG, "I2C devices: %s", s_i2c_devs);

    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = IMU_ADDR, .scl_speed_hz = 400000};
    uint8_t reg = 0x00, who = 0;
    if (i2c_master_bus_add_device(s_i2c, &dev, &s_imu) == ESP_OK &&
        i2c_master_transmit_receive(s_imu, &reg, 1, &who, 1, 50) == ESP_OK && who == 0x05) {
        // the temperature only updates while a sensor runs: accelerometer at its slowest rate
        uint8_t ctrl1[] = {0x02, 0x40}, ctrl2[] = {0x03, 0x0f}, ctrl7[] = {0x08, 0x01};
        i2c_master_transmit(s_imu, ctrl1, 2, 50);
        i2c_master_transmit(s_imu, ctrl2, 2, 50);
        i2c_master_transmit(s_imu, ctrl7, 2, 50);
    } else {
        ESP_LOGW(TAG, "no QMI8658 (who=0x%02x)", who);
        s_imu = NULL;
    }
}

// Speaker only: the ES7210 mics share the I2S port but nothing here uses them.
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
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
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
    power_init();
    i2c_init();
    mics_sleep();
    backlight_init();
    lcd_init();
    audio_init();
}

esp_codec_dev_handle_t board_speaker(void)
{
    return s_spk;
}

void board_amp(bool on)
{
    gpio_set_level(PIN_PA, on);
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---------------------------------------------------------------- keys

static key_fn s_key_fn;
static volatile int s_held_ms;

static void keys_task(void *arg)
{
    static const gpio_num_t pins[3] = {PIN_KEY_POWER, PIN_KEY_UP, PIN_KEY_DOWN};
    int held_ms[3] = {0};
    bool long_sent[3] = {0};
    // a key held at boot (BOOT is GPIO0) must be released before it counts
    bool armed[3];
    for (int i = 0; i < 3; i++) {
        armed[i] = gpio_get_level(pins[i]) == 1;
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        for (int i = 0; i < 3; i++) {
            bool down = gpio_get_level(pins[i]) == 0;
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
                    s_key_fn(i, PRESS_REPEAT); // volume keys repeat while held
                }
            } else {
                if (i == KEY_POWER && held_ms[i] >= HOLD_MS && !long_sent[i]) {
                    s_key_fn(i, PRESS_CANCEL); // let go before the power-off
                } else if (held_ms[i] >= 40 && !long_sent[i]) {
                    s_key_fn(i, PRESS_SHORT);
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
        .pin_bit_mask = 1ULL << PIN_KEY_POWER | 1ULL << PIN_KEY_UP | 1ULL << PIN_KEY_DOWN,
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
// warning. While charging the voltage reads high, so the figure runs ahead then.
static const int s_curve_mv[11] = {3050, 3500, 3640, 3700, 3740, 3780, 3820, 3860, 3900, 3950, 4000};

void board_battery(battery_t *out)
{
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
    if (out->mv <= s_curve_mv[0]) {
        out->percent = 0;
    } else if (out->mv >= s_curve_mv[10]) {
        out->percent = 100;
    } else {
        for (int i = 0; i < 10; i++) {
            if (out->mv < s_curve_mv[i + 1]) {
                out->percent = i * 10 + 10 * (out->mv - s_curve_mv[i]) / (s_curve_mv[i + 1] - s_curve_mv[i]);
                break;
            }
        }
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

// The ST7789 turns the picture itself (MADCTL); its memory is 240 x 320, so the two
// rotations that count from its far end need an 80 line offset (the same offsets as the
// radio firmware on this board: 80 across at 90 degrees, 80 down at 180).
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
    static const struct {
        bool swap, mx, my;
        int gx, gy;
    } t[4] = {{false, false, false, 0, 0}, {true, false, true, 80, 0}, {false, true, true, 0, 80}, {true, true, false, 0, 0}};
    int r = s_rot_want;
    if (r == s_rot || !s_panel) {
        return false;
    }
    esp_lcd_panel_swap_xy(s_panel, t[r].swap);
    esp_lcd_panel_mirror(s_panel, t[r].mx, t[r].my);
    esp_lcd_panel_set_gap(s_panel, t[r].gx, t[r].gy);
    s_rot = r;
    return true;
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
