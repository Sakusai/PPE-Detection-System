#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "cJSON.h"

#define I2C_PORT         I2C_NUM_0
#define I2C_SDA_PIN      GPIO_NUM_5
#define I2C_SCL_PIN      GPIO_NUM_6
#define I2C_FREQ_HZ      400000
#define SSCMA_ADDR       0x62

#define FEATURE_TRANSPORT   0x10
#define CMD_READ            0x01
#define CMD_WRITE           0x02
#define CMD_AVAILABLE       0x03

#define MAX_PL_LEN          250
#define WAIT_DELAY_MS       2

#define TARGET_PERSON_ID     2    
#define TARGET_GLASSES_ID    1 
#define CONFIDENCE_THRESHOLD 70   

#define CONFIRMATION_FRAMES  5    
#define ABSENCE_FRAMES       5  

static const char *TAG = "PPE_SYSTEM";

typedef enum {
    STATE_ABSENT,
    STATE_PRESENT
} presence_state_t;

typedef struct {
    presence_state_t state;
    int detection_counter;
    int absence_counter;
} tracker_t;

static tracker_t g_glasses_tracker = { .state = STATE_ABSENT, .detection_counter = 0, .absence_counter = 0 };
static tracker_t g_person_tracker  = { .state = STATE_ABSENT, .detection_counter = 0, .absence_counter = 0 };

static esp_err_t sscma_i2c_ctrl(uint8_t cmd, uint16_t len, const uint8_t *payload)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (SSCMA_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, FEATURE_TRANSPORT, true);
    i2c_master_write_byte(h, cmd, true);
    i2c_master_write_byte(h, (uint8_t)(len >> 8), true);
    i2c_master_write_byte(h, (uint8_t)(len & 0xFF), true);
    if (payload && len > 0) {
        i2c_master_write(h, (uint8_t *)payload, len, true);
    }
    i2c_master_write_byte(h, 0x00, true);
    i2c_master_write_byte(h, 0x00, true);
    i2c_master_stop(h);

    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(h);
    return err;
}

static int sscma_i2c_available(void)
{
    vTaskDelay(pdMS_TO_TICKS(WAIT_DELAY_MS));
    if (sscma_i2c_ctrl(CMD_AVAILABLE, 0, NULL) != ESP_OK) return -1;

    vTaskDelay(pdMS_TO_TICKS(WAIT_DELAY_MS));
    uint8_t buf[2] = {0};
    if (i2c_master_read_from_device(I2C_PORT, SSCMA_ADDR, buf, 2, pdMS_TO_TICKS(200)) != ESP_OK) {
        return -1;
    }
    return (buf[0] << 8) | buf[1];
}

static esp_err_t sscma_i2c_read(uint8_t *data, uint16_t length)
{
    uint16_t offset = 0;
    while (offset < length) {
        uint16_t chunk = (length - offset) > MAX_PL_LEN ? MAX_PL_LEN : (length - offset);

        vTaskDelay(pdMS_TO_TICKS(WAIT_DELAY_MS));
        if (sscma_i2c_ctrl(CMD_READ, chunk, NULL) != ESP_OK) return ESP_FAIL;

        vTaskDelay(pdMS_TO_TICKS(WAIT_DELAY_MS));
        esp_err_t err = i2c_master_read_from_device(I2C_PORT, SSCMA_ADDR, data + offset, chunk, pdMS_TO_TICKS(300));
        if (err != ESP_OK) return err;

        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t sscma_i2c_write(const uint8_t *data, uint16_t length)
{
    uint16_t offset = 0;
    while (offset < length) {
        uint16_t chunk = (length - offset) > MAX_PL_LEN ? MAX_PL_LEN : (length - offset);
        vTaskDelay(pdMS_TO_TICKS(WAIT_DELAY_MS));
        esp_err_t err = sscma_i2c_ctrl(CMD_WRITE, chunk, data + offset);
        if (err != ESP_OK) return err;
        offset += chunk;
    }
    return ESP_OK;
}

static void sscma_send_at(const char *at_body)
{
    char cmd[256];
    int len = snprintf(cmd, sizeof(cmd), "AT+%s\r\n", at_body);
    sscma_i2c_write((uint8_t *)cmd, len);
}

static bool sscma_read_raw(char *out, size_t out_size, int timeout_ms)
{
    size_t total = 0;
    out[0] = '\0';
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        int avail = sscma_i2c_available();
        if (avail > 0) {
            int to_read = avail;
            if ((int)(total + to_read) >= (int)out_size) to_read = out_size - total - 1;
            if (to_read > 0) {
                if (sscma_i2c_read((uint8_t *)(out + total), to_read) == ESP_OK) {
                    total += to_read;
                    out[total] = '\0';
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return total > 0;
}

static void update_tracker(tracker_t *tracker, bool detected, int score, const char *label_present, const char *label_absent) {
    if (detected) {
        tracker->absence_counter = 0;
        tracker->detection_counter++;

        if (tracker->state == STATE_ABSENT) {
            if (tracker->detection_counter >= CONFIRMATION_FRAMES) {
                tracker->state = STATE_PRESENT;
                ESP_LOGW(TAG, "🟢 %s (Confiance: %d%%)", label_present, score);
            }
        }
    } else {
        tracker->detection_counter = 0;

        if (tracker->state == STATE_PRESENT) {
            tracker->absence_counter++;

            if (tracker->absence_counter >= ABSENCE_FRAMES) {
                tracker->state = STATE_ABSENT;
                ESP_LOGE(TAG, "🔴 %s", label_absent);
            }
        }
    }
}

static void parse_sscma_response(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *boxes = cJSON_GetObjectItem(root, "boxes");
    if (!boxes) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (data) {
            boxes = cJSON_GetObjectItem(data, "boxes");
        }
    }

    bool glasses_found = false;
    bool person_found  = false;
    int glasses_conf   = 0;
    int person_conf    = 0;

    if (boxes && cJSON_IsArray(boxes)) {
        int count = cJSON_GetArraySize(boxes);

        for (int i = 0; i < count; i++) {
            cJSON *box = cJSON_GetArrayItem(boxes, i);
            if (cJSON_IsArray(box) && cJSON_GetArraySize(box) >= 6) {
                int score  = cJSON_GetArrayItem(box, 4)->valueint;
                int target = cJSON_GetArrayItem(box, 5)->valueint;

                // Filtrage Glasses (Target ID 2)
                if (target == TARGET_GLASSES_ID && score >= CONFIDENCE_THRESHOLD) {
                    glasses_found = true;
                    if (score > glasses_conf) glasses_conf = score;
                }

                // Filtrage Person (Target ID 1)
                if (target == TARGET_PERSON_ID && score >= CONFIDENCE_THRESHOLD) {
                    person_found = true;
                    if (score > person_conf) person_conf = score;
                }
            }
        }
    }

    update_tracker(&g_person_tracker, person_found, person_conf, 
                   "PERSON DETECTED", "PERSON OUT");

    update_tracker(&g_glasses_tracker, glasses_found, glasses_conf, 
                   "CONFIRMED PRESENCE OF GLASSES !", "GLASSES REMOVED / MISSING");

    cJSON_Delete(root);
}

void app_main(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));

    ESP_LOGI(TAG, "Start PPE DETECTION");
    vTaskDelay(pdMS_TO_TICKS(2000));

    sscma_send_at("INVOKE=1");

    static char response[4096]; 

    while (1) {
        if (sscma_read_raw(response, sizeof(response), 200)) {
            parse_sscma_response(response);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}