/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019-2025 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP32 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "soc/cpu.h"
#include "esp_http_client.h"
#include "esp_at_core.h"
#include "esp_at.h"
#include "at_wifi_rhzl.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#ifdef CONFIG_AT_RHZL_COMMAND_SUPPORT

#define SOFTWARE_VERSION           "100.00.03"
#define ZHIDA_VERSION              "WLT_ESP32_Zhida_Z13"
#define EXAMPLE_ESP_WIFI_SSID      "Hoisting"
#define EXAMPLE_ESP_WIFI_PASS      "jx999999"
#define ESP_WIFI_AP_SSID           "espressif_rhdk"
#define ESP_WIFI_AP_PASS           "dk666666"
#define ESP_WIFI_AP_CHANNEL         6
#define ESP_WIFI_MAX_STA_CONN       16
#define EXAMPLE_ESP_MAXIMUM_RETRY   5
#define TEMP_BUFFER_SIZE            128
#define ESP_AT_SCAN_LIST_SIZE       16

static const char *TAG = "rhat";
static const char *ATE0 = "ATE0\r\n";
static int s_retry_num = 0;
int g_wlmode = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group = NULL;
TaskHandle_t taskhandle = NULL;
esp_event_handler_instance_t instance_any_id;
esp_event_handler_instance_t instance_got_ip;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

void show_version(void)
{
    ESP_LOGI("", "*****************************");
    ESP_LOGI("", "SV:"SOFTWARE_VERSION);
    ESP_LOGI("", "*****************************");
}

bool wlmode_is_std(void)
{
    if (g_wlmode == WLMODE_STD)
        return true;
    else
        return false;
}

int nvs_write_data_to_flash(struct store_para *para)
{
    nvs_handle handle;
    static const char *NVS_CUSTOMER = "rhzd data";
    static const char *DATA1 = "net para";

    ESP_LOGI("NV", "write wifi mode:%d\r\n", para->mode);
    ESP_LOGI("NV", "write ssid:%s passwd:%s\r\n", para->ssid, para->password);
    ESP_LOGI("NV", "write ip:%s port:%d\r\n", para->ip, para->port);

    ESP_ERROR_CHECK(nvs_open(NVS_CUSTOMER, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_blob(handle, DATA1, para, sizeof(struct store_para)));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);

    return 0;
}

int nvs_read_data_from_flash(struct store_para *para)
{
    nvs_handle handle;
    uint32_t len = 0;
    static const char *NVS_CUSTOMER = "rhzd data";
    static const char *DATA1 = "net para";

    len = sizeof(struct store_para);
    memset(para, 0x0, len);

    ESP_ERROR_CHECK(nvs_open(NVS_CUSTOMER, NVS_READWRITE, &handle));
    if (nvs_get_blob(handle, DATA1, para, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI("NV", "param not found\n");
        return 0;
    }

    ESP_LOGI("NV", "read wifi mode:%d\r\n", para->mode);
    ESP_LOGI("NV", "read ssid:%s passwd:%s\r\n", para->ssid, para->password);
    ESP_LOGI("NV", "read ip:%s port:%d\r\n", para->ip, para->port);

    nvs_close(handle);
    return 0;
}

static uint8_t at_set_default_netcfg(void)
{
    struct store_para net_para = {0};

    nvs_read_data_from_flash(&net_para);
    if (net_para.ip && net_para.port) {
        // creat socket
        ESP_LOGI(TAG, "Set default net: %s, port %d\n", net_para.ip, net_para.port);
        taskhandle = socket_open(&net_para);
    } else {
        ESP_LOGW(TAG, "No net para\n");
    }

    return ESP_AT_RESULT_CODE_OK;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    uint8_t out_str[128] = {0};
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                s_retry_num = 0;
                esp_at_rhzl_write_data((uint8_t *)"+IND=WICI\r\n", strlen("+IND=WICI\r\n"));
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGE(TAG, "retry to connect to the AP");
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    esp_at_rhzl_write_data((uint8_t *)"+IND=WIDI,200\r\n", strlen("+IND=WIDI,200\r\n"));
                    if (taskhandle)
                        socket_close(&taskhandle);
                    //esp_wifi_stop();
                }
                ESP_LOGE(TAG,"connect to the AP fail");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        switch(event_id) {
            case IP_EVENT_STA_GOT_IP:
                sprintf((char *)out_str, "+IND=GTIP,"IPSTR"\r\n", IP2STR(&event->ip_info.ip));
                ESP_LOGI(TAG, "%s", out_str);

                s_retry_num = 0;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                esp_at_rhzl_write_data((uint8_t *)out_str, strlen((char *)out_str));
                vTaskDelay(500 / portTICK_PERIOD_MS);
                at_set_default_netcfg();
                break;
            case IP_EVENT_STA_LOST_IP:
                esp_at_rhzl_write_data((uint8_t *)"+IND=LOSIP\r\n", strlen("+IND=LOSIP\r\n"));
                break;
            default:
                break;
        }
    }
}

static uint8_t at_event_register_call(void)
{
    if (s_wifi_event_group) {
        ESP_LOGW(TAG,"at event call already registered");
        return 0;
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    return 0;
}

static int esp_rhzl_wifi_start(uint8_t *ssid, uint8_t *passwd)
{
    wifi_config_t wifi_config = {0};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // STA config
    strcpy((char*)wifi_config.sta.ssid, (char*)ssid);
    strcpy((char*)wifi_config.sta.password, (char*)passwd);
    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WEP;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // AP config
    strcpy((char*)wifi_config.ap.ssid, (char*)ESP_WIFI_AP_SSID);
    wifi_config.ap.ssid_len = strlen(ESP_WIFI_AP_SSID);
    strcpy((char*)wifi_config.ap.password, (char*)ESP_WIFI_AP_PASS);
    wifi_config.ap.channel = ESP_WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = ESP_WIFI_MAX_STA_CONN;
    if (strlen(ESP_WIFI_AP_PASS) == 0) {
        memset(wifi_config.ap.password, 0, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ssid, passwd);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s, password:%s",
                 ssid, passwd);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
    return 0;
}

uint8_t esp_at_rhzl_init(void)
{
    struct store_para net_para = {0};

    esp_at_port_read_data((uint8_t *)ATE0, strlen(ATE0));
    esp_at_rhzl_write_data((uint8_t *)ZHIDA_VERSION"\r\n", strlen(ZHIDA_VERSION"\r\n"));
    esp_at_rhzl_write_data((uint8_t *)"SV:"SOFTWARE_VERSION"\r\n", strlen("SV:"SOFTWARE_VERSION"\r\n"));
    show_version();

    nvs_read_data_from_flash(&net_para);
    g_wlmode = net_para.mode;
    if (net_para.mode == WLMODE_STD) {
        ESP_LOGI(TAG, "STD WIFI mode\n");
    } else if (net_para.ssid && net_para.password) {
        ESP_LOGI(TAG, "RHZD WIFI mode\n");
        at_event_register_call();
        esp_rhzl_wifi_start((uint8_t *)net_para.ssid, (uint8_t *)net_para.password);
    } else {
        ESP_LOGW(TAG, "No net para of wifi\n");
    }

    return 0;
}

static uint8_t at_query_wlan(uint8_t *cmd_name)
{
    ESP_LOGE(TAG, "at_query_cmd_wlan");

    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "%s: RHZL test\r\n", cmd_name);
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_wlan(uint8_t para_num)
{
    int32_t cnt = 0;
    uint8_t *ssid = NULL;
    uint8_t *passwd = NULL;
    struct store_para net_para = {0};

    ESP_LOGI(TAG, "at_setup_wlan");

    esp_wifi_stop();
    //at_event_register_call();
    if (esp_at_get_para_as_str(cnt++, &ssid) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get ssid: %s", ssid);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (esp_at_get_para_as_str(cnt++, &passwd) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get password: %s", passwd);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    nvs_read_data_from_flash(&net_para);
    if (strncmp((const char *)&net_para.ssid, (const char *)ssid, WIFI_SSID_MAX_LEN) ||
        strncmp((const char *)&net_para.password, (const char*)passwd, WIFI_PWD_MAX_LEN) ||
        net_para.mode != WLMODE_RHZD) {
        // If you use a custom command to set WIFI, it is considered to be in RHZD mode
        net_para.mode = WLMODE_RHZD;
        memcpy(&net_para.ssid, ssid, strlen((const char *)ssid) + 1);
        memcpy(&net_para.password, passwd, strlen((const char *)passwd) + 1);
        nvs_write_data_to_flash(&net_para);
    }

    at_event_register_call();
    esp_rhzl_wifi_start(ssid, passwd);
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_getwlan(uint8_t *cmd_name)
{
    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    struct store_para net_para = {0};

    nvs_read_data_from_flash(&net_para);
    ESP_LOGI(TAG, "getwlan SSID:%s password:%s",
                 net_para.ssid, net_para.password);

    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "OK=%s,%s\r\n", net_para.ssid, net_para.password);
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_stopwlan(uint8_t *cmd_name)
{
    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
    ESP_ERROR_CHECK(esp_wifi_stop());
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_setnet(uint8_t para_num)
{
    int cnt = 2;  //ignore the first of 2
    int32_t port = 0;
    uint8_t *ip_addr = NULL;
    struct store_para net_para = {0};

    if (esp_at_get_para_as_str(cnt++, &ip_addr) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get ip_addr: %s", ip_addr);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (esp_at_get_para_as_digit(cnt++, &port) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get port: %d", port);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    nvs_read_data_from_flash(&net_para);
    if (strncmp((const char *)&net_para.ip, (const char *)ip_addr, WIFI_IP_MAX_LEN) || net_para.port != port) {
        memcpy(&net_para.ip, (const char *)ip_addr, strlen((const char *)ip_addr) + 1);
        net_para.port = port;
        nvs_write_data_to_flash(&net_para);
    }

    // creat socket
    ESP_LOGI(TAG, "Set net: %s, port %d\n", net_para.ip, net_para.port);
    if (taskhandle)
        socket_close(&taskhandle);
    taskhandle = socket_open(&net_para);
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_getnet(uint8_t *cmd_name)
{
    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    struct store_para net_para = {0};

    nvs_read_data_from_flash(&net_para);
    ESP_LOGI(TAG, "Get net: %s, port %d\n", net_para.ip, net_para.port);
    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "OK=0,0,%s,%d\r\n", net_para.ip, net_para.port);
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_stopnet(uint8_t *cmd_name)
{
    esp_wifi_stop();
    return ESP_AT_RESULT_CODE_OK;
}

// send socket data
static uint8_t at_setup_netsend(uint8_t para_num)
{
    uint8_t *data = NULL;
    int32_t len = 0;
    int32_t cnt = 0;
    TickType_t ticks_to_wait = 1000*10;

    if (esp_at_get_para_as_digit(cnt++, &len) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get data len: %d\r\n", len);
        return ESP_AT_RESULT_CODE_ERROR;
    }
    ESP_LOGI(TAG, "Get net data len: %d\r\n", len);

    if (para_num == 1) {
        data = (uint8_t *)malloc(len);
        if (!data) {
            ESP_LOGE(TAG, "malloc read buf failed\r\n");
            return ESP_AT_RESULT_CODE_ERROR;
        }

        len = uart_read_bytes(UART_NUM_1, data, len, ticks_to_wait);
        ESP_LOGI(TAG, "Next to send data, read len = %d", len);
        if (tcp_send_data((char *)data, len) < 0) {
            ESP_LOGE(TAG, "send tcp data failed\r\n");
            free(data);
            return ESP_AT_RESULT_CODE_ERROR;
        }
        free(data);
    }
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_closesocket(uint8_t *cmd_name)
{
    if (taskhandle)
        socket_close(&taskhandle);
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_wifiscan(uint8_t *cmd_name)
{
    esp_err_t ret = ESP_FAIL;
    uint16_t number = 0, i = 0;
    wifi_ap_record_t *ap_records = NULL;
    uint8_t ap_info_buf[128] = {0};
    //wifi_scan_config_t scan_config = {0};

    ap_records = (wifi_ap_record_t*) malloc(ESP_AT_SCAN_LIST_SIZE * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "ap info malloc fail");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    number = ESP_AT_SCAN_LIST_SIZE;
    memset(ap_records, 0, ESP_AT_SCAN_LIST_SIZE * sizeof(wifi_ap_record_t));

          //esp_wifi_scan_start
    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scan start fail");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    ret = esp_wifi_scan_get_ap_records(&number, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get scan fail");
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (number == 0) {
        ESP_LOGW(TAG, "There is no ap here");
        return ESP_AT_RESULT_CODE_ERROR;
    }
    ESP_LOGI(TAG, "Total APs scanned = %u", number);
    // sort ap_record according to rssi 
    ap_record_sort_by_rssi(ap_records, number);

    for(i = 0; i < number; i++) {
        sprintf((char *)ap_info_buf, "[%2d] SSID: %-33s\tSIG:%4d\tMAC: %02x:%02x:%02x:%02x:%02x:%02x\n", i, ap_records[i].ssid, \
                ap_records[i].rssi, MAC2STR(ap_records[i].bssid));
        ESP_LOGI(TAG, "%s", ap_info_buf);
        esp_at_rhzl_write_data(ap_info_buf, strlen((char*)ap_info_buf));
    }

    free(ap_records);
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_query_version(uint8_t *cmd_name)
{
    ESP_LOGI(TAG, "at_query_version");

    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "SV:"SOFTWARE_VERSION"\r\n");
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_query_wlmode(uint8_t *cmd_name)
{
    ESP_LOGE(TAG, "at_query_wlan_mode");

    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "%s: 0:STD 1:RHZD\r\n", cmd_name);
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_wlmode(uint8_t para_num)
{
    int32_t mode = 0;
    int32_t cnt = 0;
    struct store_para net_para = {0};

    if (esp_at_get_para_as_digit(cnt++, &mode) != ESP_AT_PARA_PARSE_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get wlmode: %d\r\n", mode);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (mode > WLMODE_MAX || mode < WLMODE_STD) {
        ESP_LOGE(TAG, "Invalid parameter: %d\r\n", mode);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    g_wlmode = mode;
    nvs_read_data_from_flash(&net_para);
    if (net_para.mode != mode) {
        net_para.mode = mode;
        nvs_write_data_to_flash(&net_para);
    }

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_getwlmode(uint8_t *cmd_name)
{
    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    struct store_para net_para = {0};

    nvs_read_data_from_flash(&net_para);
    ESP_LOGI(TAG, "Get wlmode: %d\n", net_para.mode);
    snprintf((char *)buffer, TEMP_BUFFER_SIZE, "wlmod:%d\r\n", net_para.mode);
    esp_at_rhzl_write_data(buffer, strlen((char *)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exec_reset(uint8_t *cmd_name)
{
    ESP_LOGI(TAG, "Reboot ...");
    esp_restart();
    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_struct s_at_rhzl_cmd[] = {
    {"+SETWLAN",            NULL, at_query_wlan, at_setup_wlan, NULL},      // set ssid & password of wlan
    {"+GETWLAN",            NULL, NULL, NULL, at_exec_getwlan},             // get ssid & password of wlan
    {"+STOPWLAN",           NULL, NULL, NULL, at_exec_stopwlan},            // close wlan
    {"+SETNET",             NULL, NULL, at_setup_setnet, NULL},             // set host IP & Port
    {"+GETNET",             NULL, NULL, NULL, at_exec_getnet},              // get net para
    {"+STOPNET",            NULL, NULL, NULL, at_exec_stopnet},             // close power on auto connect
    {"+NETSEND",            NULL, NULL, at_setup_netsend, NULL},            // send socket data
    {"+CLOSESOCKET",        NULL, NULL, NULL, at_exec_closesocket},         // close socket connect
    {"+WIFISTARTSCANNING",  NULL, NULL, NULL, at_exec_wifiscan},            // scan ap list
    {"+GETVERSION",         NULL, at_query_version, NULL, NULL},            // get rhzd software version
    {"+SETWLMODE",          NULL, at_query_wlmode, at_setup_wlmode, NULL},  // set wlan mode
    {"+GETWLMODE",          NULL, NULL, NULL, at_exec_getwlmode},           // get wlan mode
    {"+RESET",              NULL, NULL, NULL, at_exec_reset},               // reset
};

bool esp_at_rhzl_cmd_regist(void)
{
    return esp_at_custom_cmd_array_regist(s_at_rhzl_cmd, sizeof(s_at_rhzl_cmd) / sizeof(s_at_rhzl_cmd[0]));
}
#endif
