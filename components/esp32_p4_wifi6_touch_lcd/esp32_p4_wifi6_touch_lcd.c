#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_lcd_st7703.h"
#include "esp_lcd_jd9365.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "bsp/display.h"
#include "bsp/brightness_curve.h"
#include "bsp/touch.h"
#include "esp_lcd_touch_gt911.h"
#include "bsp_err_check.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "ESP32_P4_4B";

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *disp_indev = NULL;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

/* False until a display really came up. bsp_display_lock() consults it so that
 * a headless boot (no usable panel, see main/board_profile.c) turns every
 * display-lock site in the application into a clean "lock not acquired" instead
 * of the esp_lvgl_port NULL-mutex assert. Never cleared: nothing tears a
 * started display down. */
static bool s_display_started = false;

sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler
static bool i2c_initialized = false;
static TaskHandle_t usb_host_task;  // USB Host Library task
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0))
static i2c_master_bus_handle_t i2c_handle = NULL;  // I2C Handle
#endif
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */
#define BSP_ES7210_CODEC_ADDR  ES7210_CODEC_DEFAULT_ADDR

/* Can be used for `i2s_std_gpio_config_t` and/or `i2s_std_config_t` initialization */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/* This configuration is used by default in `bsp_extra_audio_init()` */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return i2c_handle;
}

static esp_err_t bsp_enable_ldo_vo4(void)
{
    static esp_ldo_channel_handle_t vo4_chan = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };

    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &vo4_chan), TAG, "Acquire LDO VO4 channel failed");
    ESP_LOGI(TAG, "LDO VO4 set to 3300mV");

    return ESP_OK;
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 64 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;


    ESP_RETURN_ON_ERROR(bsp_enable_ldo_vo4(), TAG, "DSI PHY power failed");

    const sdmmc_slot_config_t slot_config = {
        /* SD card is connected to Slot 0 pins. Slot 0 uses IO MUX, so not specifying the pins here */
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };



    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

/**************************************************************************************************
 *
 * I2S Audio Function
 *
 **************************************************************************************************/
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S enabling failed");
    }
    if (i2s_rx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S enabling failed");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK_GOTO(i2s_data_if, err);

    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
    }

    return ret;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    BSP_NULL_CHECK(es8311_dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    BSP_NULL_CHECK(es7210_dev, NULL);

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

// Bit number used to represent command and parameter
#define LCD_LEDC_CH            CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

esp_err_t bsp_display_brightness_init(void)
{
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = 1 }
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&LCD_backlight_channel));

    return ESP_OK;
}

/* Panel table
 *
 * Every GPIO, the DSI PHY LDO channel and voltage, the lane count, the LEDC
 * timer/channel/frequency/polarity and the DBI command widths are identical on
 * all three boards and stay constants above. Only what actually differs is a
 * row here.
 *
 * The two round rows instantiate ONE init macro with the page-1 register 0x40
 * byte as its parameter, so each row owns an immutable table. The square row
 * passes init_cmds = NULL and the ST7703 driver runs its own default sequence,
 * which is exactly what this BSP did before the table existed. */

typedef esp_err_t (*bsp_panel_new_fn)(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t *dev_cfg,
                                      esp_lcd_panel_handle_t *ret_panel);

typedef struct {
    const char        *name;
    bsp_panel_new_fn   panel_new;
    const void        *init_cmds;      /* jd9365_lcd_init_cmd_t[], NULL for ST7703 */
    uint16_t           init_cmds_size;
    uint16_t           h_res;
    uint16_t           v_res;
    uint32_t           lane_rate_mbps;
    uint32_t           dpi_clock_mhz;
    uint16_t           hbp, hpw, hfp;
    uint16_t           vbp, vpw, vfp;
    uint8_t            bright_floor_pct;  /* first point of the brightness curve */
    /* Base orientation, applied ONCE at init as hardware MADCTL state through
     * lvgl_port_display_cfg_t.rotation. Never an LVGL rotation and never a
     * post-start bsp_display_rotate(). 1 means 180 degrees, which both round
     * panels need. The square row stays 0: the ST7703 driver implements
     * mirror_y but neither mirror_x nor swap_xy, so a non-zero base
     * orientation on that panel would silently do the wrong thing.
     * With LVGL rotation pinned at 0 the digitiser must carry the same
     * transform itself, so this one field drives both. */
    uint8_t            base_rot_180;
} bsp_panel_row_t;

/* JD9365 init sequence. The shared entries live in jd9365_init_table.h, which
 * is included ONCE PER ROW with the table name and the page-1 register 0x40
 * byte defined by the includer, so each row owns an immutable table and the
 * shared entries exist in one place. */
#define JD9365_TABLE_NAME s_jd9365_init_3_4
#define JD9365_REG40      0x00
#include "jd9365_init_table.h"

#define JD9365_TABLE_NAME s_jd9365_init_4c
#define JD9365_REG40      0x04
#include "jd9365_init_table.h"

static const bsp_panel_row_t s_panel_rows[] = {
    [BSP_PANEL_SQUARE_4B] = {
        .name = "4B 720x720 ST7703", .panel_new = esp_lcd_new_panel_st7703,
        .init_cmds = NULL, .init_cmds_size = 0,
        .h_res = 720, .v_res = 720,
        .lane_rate_mbps = 480, .dpi_clock_mhz = 38,
        .hbp = 50, .hpw = 20, .hfp = 50,
        .vbp = 20, .vpw = 4,  .vfp = 20,
        .bright_floor_pct = 47,
        .base_rot_180 = 0,
    },
    [BSP_PANEL_ROUND_3_4] = {
        .name = "3.4C 800x800 JD9365", .panel_new = esp_lcd_new_panel_jd9365,
        .init_cmds = s_jd9365_init_3_4,
        .init_cmds_size = sizeof(s_jd9365_init_3_4) / sizeof(s_jd9365_init_3_4[0]),
        .h_res = 800, .v_res = 800,
        .lane_rate_mbps = 1500, .dpi_clock_mhz = 80,
        .hbp = 20, .hpw = 20, .hfp = 40,
        .vbp = 12, .vpw = 4,  .vfp = 24,
        /* Backlight is dark below about 21% duty on this panel, so 0..20 of
         * the slider was a dead band. Floor 20 puts user 0 at the last dark
         * step and spreads 1..100 over the range that actually lights. */
        .bright_floor_pct = 20,
        .base_rot_180 = 1,
    },
    [BSP_PANEL_ROUND_4C] = {
        .name = "4C 720x720 JD9365", .panel_new = esp_lcd_new_panel_jd9365,
        .init_cmds = s_jd9365_init_4c,
        .init_cmds_size = sizeof(s_jd9365_init_4c) / sizeof(s_jd9365_init_4c[0]),
        .h_res = 720, .v_res = 720,
        /* 40, not the shared 80: with the shared porches 80 MHz scans the 720
         * panel at about 132 Hz and the glass shows alternate rows holding the
         * previous frame plus brightness flicker (bench B17); 40 MHz is about
         * 66 Hz. PLL_F240M / 6, an exact divider. */
        .lane_rate_mbps = 1500, .dpi_clock_mhz = 40,
        .hbp = 20, .hpw = 20, .hfp = 40,
        .vbp = 12, .vpw = 4,  .vfp = 24,
        /* Dark below about 23% duty here, one step higher than the 3.4C. */
        .bright_floor_pct = 22,
        .base_rot_180 = 1,
    },
};

#define BSP_PANEL_ROW_COUNT (sizeof(s_panel_rows) / sizeof(s_panel_rows[0]))

#if CONFIG_NINA_FAMILY_ROUND
static int s_panel_type = BSP_PANEL_ROUND_3_4;
#else
static int s_panel_type = BSP_PANEL_SQUARE_4B;
#endif

static const bsp_panel_row_t *bsp_panel_row(void)
{
    return &s_panel_rows[s_panel_type];
}

esp_err_t bsp_display_set_panel_type(int panel_type)
{
    /* The row is read during bring-up and again by the touch and brightness
     * paths, so swapping it afterwards would leave the live panel and the
     * table disagreeing. Selection is a pre-start decision only. */
    if (s_display_started) {
        ESP_LOGW(TAG, "panel type is fixed once the display has started");
        return ESP_ERR_INVALID_STATE;
    }
    if (panel_type < 0 || panel_type >= (int)BSP_PANEL_ROW_COUNT) {
        ESP_LOGW(TAG, "unknown panel type %d, keeping %s", panel_type, bsp_panel_row()->name);
        return ESP_ERR_INVALID_ARG;
    }
    s_panel_type = panel_type;
    ESP_LOGI(TAG, "panel row selected: %s", bsp_panel_row()->name);
    return ESP_OK;
}

int bsp_display_get_h_res(void) { return bsp_panel_row()->h_res; }
int bsp_display_get_v_res(void) { return bsp_panel_row()->v_res; }

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    } else if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    /* Curve and duty both live in bsp/brightness_curve.h, which the host test
     * pins against the square panel's historical numbers. */
    const int floor_pct = bsp_panel_row()->bright_floor_pct;

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);

    uint32_t duty_cycle = (uint32_t)bsp_brightness_duty(floor_pct, brightness_percent);
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));

    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

static esp_err_t bsp_enable_dsi_phy_power(void)
{
#if BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0
    // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
    static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan), TAG, "Acquire LDO channel for DPHY failed");
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif // BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0

    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    bsp_lcd_handles_t handles;
    ret = bsp_display_new_with_handles(config, &handles);

    *ret_panel = handles.panel;
    *ret_io = handles.io;

    return ret;
}

/* DCS Read Display ID. Both vendor drivers issue this same read during their
 * own init, so it is a known working transaction on both controllers. */
#define BSP_DCS_RDDID              0x04
#define BSP_DSI_PROBE_LANE_MBPS    480

esp_err_t bsp_display_probe_rddid(uint8_t out[3])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure the reset pin before pulsing it: the panel constructor does its
     * own reset later through reset_gpio_num, but nothing has configured this
     * pin yet at probe time. */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BSP_LCD_RST,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "probe: reset pin config failed");
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(), TAG, "probe: DSI PHY power failed");

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_DSI_PROBE_LANE_MBPS,
    };
    esp_lcd_dsi_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &bus), TAG, "probe: new DSI bus failed");

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = esp_lcd_new_panel_io_dbi(bus, &dbi_config, &io);
    if (err != ESP_OK) {
        esp_lcd_del_dsi_bus(bus);
        ESP_RETURN_ON_ERROR(err, TAG, "probe: new DBI panel IO failed");
    }

    uint8_t id[3] = {0, 0, 0};
    err = esp_lcd_panel_io_rx_param(io, BSP_DCS_RDDID, id, sizeof(id));

    esp_lcd_panel_io_del(io);
    esp_lcd_del_dsi_bus(bus);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RDDID read failed: %s", esp_err_to_name(err));
        return err;
    }
    memcpy(out, id, sizeof(id));
    return ESP_OK;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles)
{
    const bsp_panel_row_t *row = bsp_panel_row();
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "Brightness init failed");
    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(), TAG, "DSI PHY power failed");
    ESP_RETURN_ON_ERROR(bsp_enable_ldo_vo4(), TAG, "DSI PHY power failed");

    /* create MIPI DSI bus first, it will initialize the DSI PHY as well */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = row->lane_rate_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "New DSI bus init failed");

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // we use DBI interface to send LCD commands and parameters
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,   // according to the LCD ILI9881C spec
        .lcd_param_bits = 8, // according to the LCD ILI9881C spec
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io), err, TAG, "New panel IO failed");

    esp_lcd_panel_handle_t disp_panel = NULL;
    ESP_LOGI(TAG, "Install MIPI DSI panel: %s", row->name);

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = row->dpi_clock_mhz,
        .virtual_channel = 0,
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
#else
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
#endif
        .num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
        .video_timing = {
            .h_size = row->h_res,
            .v_size = row->v_res,
            .hsync_back_porch  = row->hbp,
            .hsync_pulse_width = row->hpw,
            .hsync_front_porch = row->hfp,
            .vsync_back_porch  = row->vbp,
            .vsync_pulse_width = row->vpw,
            .vsync_front_porch = row->vfp,
        },
        .flags.use_dma2d = true,
    };

    /* lane_num is forwarded ONLY to the JD9365 constructor: the ST7703 vendor
     * config has no such field. */
    st7703_vendor_config_t st7703_cfg = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = { .dsi_bus = mipi_dsi_bus, .dpi_config = &dpi_config },
    };
    jd9365_vendor_config_t jd9365_cfg = {
        .init_cmds = (const jd9365_lcd_init_cmd_t *)row->init_cmds,
        .init_cmds_size = row->init_cmds_size,
        .mipi_config = { .dsi_bus = mipi_dsi_bus, .dpi_config = &dpi_config,
                         .lane_num = BSP_LCD_MIPI_DSI_LANE_NUM },
    };

    esp_lcd_panel_dev_config_t lcd_dev_config = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .bits_per_pixel = 24,
#else
        .bits_per_pixel = 16,
#endif
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config = (row->panel_new == esp_lcd_new_panel_st7703)
                             ? (void *)&st7703_cfg : (void *)&jd9365_cfg,
    };
    ESP_GOTO_ON_ERROR(row->panel_new(io, &lcd_dev_config, &disp_panel), err, TAG,
                      "New LCD panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "LCD panel reset failed");
    /* Base 180 orientation requested before init: the driver's mirror()
     * sends MADCTL now (panel still in reset, harmless) and keeps the GS/SS
     * bits in madctl_val, which init() re-sends as its own MADCTL write
     * before the vendor table and before the video stream starts. So no
     * MADCTL reaches the glass while it is streaming; the vendor bring-up
     * never writes one either. This was tried against the 4C scan-line
     * fault (bench B17) and was not the cause (the pixel clock was), but it
     * is kept as the vendor's order. The 4B row has base_rot_180 = 0 and is
     * untouched. */
    if (row->base_rot_180) {
        ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(disp_panel, true, true), err, TAG,
                          "LCD panel mirror failed");
    }
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "LCD panel init failed");

    /* Return all handles */
    ret_handles->io = io;
    ret_handles->mipi_dsi_bus = mipi_dsi_bus;
    ret_handles->panel = disp_panel;
    ret_handles->control = NULL;

    ESP_LOGI(TAG, "Display initialized");

    return ret;

err:
    if (disp_panel) {
        esp_lcd_panel_del(disp_panel);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    if (mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(mipi_dsi_bus);
    }
    return ret;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    /* Initialize touch */
    const bsp_panel_row_t *row = bsp_panel_row();

    /* x_max/y_max must be the LIVE panel resolution: esp_lcd_touch computes
     * x = x_max - x for mirror_x, so a stale 720 on an 800 px panel lands every
     * mirrored touch 80 px off.
     *
     * The transform carries the base orientation. With MADCTL 180 as hardware
     * state and LVGL rotation pinned at 0, the port compensates nothing, so an
     * uncompensated digitiser puts every touch on a round board at the
     * antipode. That is a first-boot symptom which reads as a broken panel. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = bsp_display_get_h_res(),
        .y_max = bsp_display_get_v_res(),
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = (row->base_rot_180 != 0),
            .mirror_y = (row->base_rot_180 != 0),
        },
    };

    /* The retry ladder below toggles BSP_LCD_TOUCH_RST directly. Nothing else
     * in this component configures that pin as an output, so without this every
     * "hard reset" in rounds 2 to 5 was a no-op and the ladder burned 3.5 s for
     * nothing. */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BSP_LCD_TOUCH_RST,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&rst_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "touch reset pin config failed; the retry ladder cannot reset GT911");
    } else {
        /* Drive it HIGH straight away. gpio_config() on an output leaves the
         * output register at its 0 default, which would hold GT911 in reset
         * from here until the driver's own reset pulse; the board previously
         * idled this pin high through its pull-up. Matching that keeps the
         * square board's pre-init state exactly as it was. */
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
    }

    /*
     * GT911 I2C address depends on the INT pin state during reset:
     *   INT LOW  → 0x5D (primary)
     *   INT HIGH → 0x14 (backup)
     * When INT is not connected (GPIO_NUM_NC), the pin floats and the
     * address is unpredictable per board.  Try both addresses.
     *
     * Some boards also need extra time after cold power-on before the
     * GT911's I2C interface is ready.  Retry with increasing delays.
     */
    const uint16_t gt911_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };
    const int num_addrs = sizeof(gt911_addrs) / sizeof(gt911_addrs[0]);
    const int max_rounds = 5;

    esp_err_t ret = ESP_FAIL;
    for (int round = 0; round < max_rounds; round++) {
        for (int addr_idx = 0; addr_idx < num_addrs; addr_idx++) {
            esp_lcd_panel_io_handle_t tp_io_handle = NULL;
            esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
            tp_io_config.dev_addr = gt911_addrs[addr_idx];
            tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
            ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");

            ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "GT911 touch initialized at I2C addr 0x%02X (round %d)", gt911_addrs[addr_idx], round + 1);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "GT911 not found at 0x%02X (round %d/%d)", gt911_addrs[addr_idx], round + 1, max_rounds);
            esp_lcd_panel_io_del(tp_io_handle);
        }

        /* Hard reset GT911 and wait progressively longer: 500, 750, 1000, 1250ms */
        int delay_ms = 500 + (round * 250);
        ESP_LOGW(TAG, "Resetting GT911, waiting %dms before retry...", delay_ms);
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGE(TAG, "GT911 touch init failed after %d rounds at both I2C addresses", max_rounds);
    return ret;
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
/* Orientation sink for the round rows. esp_lvgl_port applies its rotation at
 * add time and on every lv_display_set_rotation() through
 * esp_lcd_panel_swap_xy()/esp_lcd_panel_mirror() on the control handle, and
 * on the JD9365 each of those is a MADCTL write after the video stream is up,
 * which the vendor bring-up never does (bench B17; not the cause of the 4C
 * scan-line fault, that was the pixel clock, but kept as the vendor's order).
 * The base 180 flip is baked into the panel's init MADCTL instead (see
 * bsp_display_new_with_handles), and user rotation runs on the PPA, so the
 * port's calls have nothing to do. Returning ESP_OK keeps the boot log quiet;
 * a NULL op would make esp_lcd log "not supported" on every call. */
static esp_err_t orientation_sink_mirror(esp_lcd_panel_t *panel, bool x, bool y)
{
    (void)panel; (void)x; (void)y;
    return ESP_OK;
}
static esp_err_t orientation_sink_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    (void)panel; (void)swap;
    return ESP_OK;
}
static esp_lcd_panel_t s_orientation_sink = {
    .mirror  = orientation_sink_mirror,
    .swap_xy = orientation_sink_swap_xy,
};

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    const bsp_panel_row_t *row = bsp_panel_row();
    bsp_lcd_handles_t lcd_panels;
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new_with_handles(NULL, &lcd_panels));

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_panels.io,
        .panel_handle = lcd_panels.panel,
        /* Round rows: the port's orientation writes go to the sink (see
         * s_orientation_sink). Square: NULL, the port drives the panel as shipped. */
        .control_handle = row->base_rot_180 ? &s_orientation_sink : NULL,
        .buffer_size = cfg->buffer_size,
        .double_buffer = cfg->double_buffer,
        .hres = bsp_display_get_h_res(),
        .vres = bsp_display_get_v_res(),
        .monochrome = false,
        /* Base orientation is already in the panel's init MADCTL (see
         * bsp_display_new_with_handles); a mirror here would make the port
         * write MADCTL again after the stream is running. */
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .color_format = LV_COLOR_FORMAT_RGB888,
#else
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
#endif
        .flags = {
            .buff_dma = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .sw_rotate = false,                /* Avoid tearing is not supported for SW rotation */
#else
            .sw_rotate = cfg->flags.sw_rotate, /* Only SW rotation is supported for 90° and 270° */
#endif
#if CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH
            .full_refresh = true,
#elif CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE
            .direct_mode = true,
#endif
        }
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .avoid_tearing = true,
#else
            .avoid_tearing = false,
#endif
        }
    };

    return lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
}

static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    esp_lcd_touch_handle_t tp;
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));
    assert(tp);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = (bsp_display_get_h_res() * 50),
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = false,
            .sw_rotate = true,
        }
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&cfg->lvgl_port_cfg));

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    /* The panel and LVGL are up from here, so the mutex exists and the lock
     * gate opens now rather than after touch. A GT911 failure below must not
     * leave every display-lock site in the application closed against a screen
     * that is actually running. */
    s_display_started = true;

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(disp), NULL);

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_started(void)
{
    return s_display_started;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    /* No display, no LVGL mutex. Returning false is the honest answer and it is
     * what every caller in this project already handles; calling through would
     * hit the esp_lvgl_port assert instead. */
    if (!s_display_started) {
        return false;
    }
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    if (!s_display_started) {
        return;
    }
    lvgl_port_unlock();
}

#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

static void usb_lib_task(void *arg)
{
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
        }
    }
}

esp_err_t bsp_usb_host_start(bsp_usb_host_power_mode_t mode, bool limit_500mA)
{
    //Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    BSP_ERROR_CHECK_RETURN_ERR(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, &usb_host_task) != pdTRUE) {
        ESP_LOGE(TAG, "Creating USB host lib task failed");
        abort();
    }

    return ESP_OK;
}

esp_err_t bsp_usb_host_stop(void)
{
    usb_host_uninstall();
    if (usb_host_task) {
        vTaskSuspend(usb_host_task);
        vTaskDelete(usb_host_task);
    }
    return ESP_OK;
}
