#include "board_power.h"
#include "sdkconfig.h"

#ifdef CONFIG_ADSB_BOARD_TBEAM_SUPREME_SX1262

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

enum {
    PMU_ADDRESS = 0x34,
    PMU_REG_CHIP_ID = 0x03,
    PMU_REG_ADC_CHANNEL = 0x30,
    PMU_REG_LDO_ENABLE = 0x90,
    PMU_REG_ALDO1_VOLTAGE = 0x92,
    PMU_REG_ALDO3_VOLTAGE = 0x94,
    PMU_CHIP_ID = 0x4a,
    PMU_TS_MEASURE = 1u << 1,
    PMU_ALDO1_ENABLE = 1u << 0,
    PMU_ALDO3_ENABLE = 1u << 2,
    PMU_ALDO_3300_CODE = 0x1c,
};

static const char *TAG = "board-power";

static esp_err_t read_register(uint8_t reg, uint8_t *value) {
    return i2c_master_write_read_device(I2C_NUM_1, PMU_ADDRESS, &reg, 1, value, 1,
                                        pdMS_TO_TICKS(100));
}

static esp_err_t write_register(uint8_t reg, uint8_t value) {
    uint8_t bytes[] = {reg, value};
    return i2c_master_write_to_device(I2C_NUM_1, PMU_ADDRESS, bytes, sizeof(bytes),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t update_register(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current, verified;
    ESP_RETURN_ON_ERROR(read_register(reg, &current), TAG, "read register 0x%02x", reg);
    current = (current & (uint8_t)~mask) | (value & mask);
    ESP_RETURN_ON_ERROR(write_register(reg, current), TAG, "write register 0x%02x", reg);
    ESP_RETURN_ON_ERROR(read_register(reg, &verified), TAG, "verify register 0x%02x", reg);
    if ((verified & mask) != (value & mask)) {
        ESP_LOGE(TAG, "register 0x%02x did not retain the requested value", reg);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t board_power_init(void) {
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 42,
        .scl_io_num = 41,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_1, &config), TAG, "configure PMU I2C");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_1, config.mode, 0, 0, 0), TAG,
                        "install PMU I2C driver");

    uint8_t chip_id;
    ESP_RETURN_ON_ERROR(read_register(PMU_REG_CHIP_ID, &chip_id), TAG, "read AXP2101 ID");
    if (chip_id != PMU_CHIP_ID) {
        ESP_LOGE(TAG, "expected AXP2101 at 0x%02x, found ID 0x%02x", PMU_ADDRESS, chip_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(update_register(PMU_REG_ADC_CHANNEL, PMU_TS_MEASURE, 0), TAG,
                        "disable unsupported battery-temperature input");
    ESP_RETURN_ON_ERROR(update_register(PMU_REG_ALDO1_VOLTAGE, 0x1f, PMU_ALDO_3300_CODE), TAG,
                        "set display rail to 3.3 V");
    ESP_RETURN_ON_ERROR(update_register(PMU_REG_ALDO3_VOLTAGE, 0x1f, PMU_ALDO_3300_CODE), TAG,
                        "set radio rail to 3.3 V");
    ESP_RETURN_ON_ERROR(update_register(PMU_REG_LDO_ENABLE,
                                        PMU_ALDO1_ENABLE | PMU_ALDO3_ENABLE,
                                        PMU_ALDO1_ENABLE | PMU_ALDO3_ENABLE),
                        TAG, "enable display and radio rails");
    return ESP_OK;
}

#else

esp_err_t board_power_init(void) {
    return ESP_OK;
}

#endif
