/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "at_wifi_rhzl.h"
#include "driver/uart.h"
#include "esp_at_core.h"
#include "utlist.h"
#include "nvs.h"

#define BUFFER_RX_MAX_LEN           256
#define BUFFER_INDEX_MAX_LEN        32
#define BUFFER_OUT_CMD_LEN          (BUFFER_RX_MAX_LEN + BUFFER_INDEX_MAX_LEN)

static const char *TAG = "wifi";

enum wifi_status {
    WIFI_UNKNOW = 0,
    WIFI_READY,
    WIFI_DISCONNECT,
    WIFI_CONNECTTING,
    WIFI_CONNECTED,
    WIFI_RETRY,
};

struct wifi_info {
    enum wifi_status status;
    int handle;
    int retry_num;
};

struct buf_el {
    int id;
    int len;
    char *buf;
    struct buf_el *next, *prev;
};

struct buf_el *h_list = NULL;
struct wifi_info wifi = {0};

SemaphoreHandle_t xSemaphore = NULL;

int32_t esp_at_rhzl_write_data(uint8_t*data, int32_t len)
{
    uint32_t length = 0;

    ESP_LOGI(TAG, "O: %s", data);
    length = uart_write_bytes(UART_NUM_1,(char*)data, len);
    if (length <= 0) {
        ESP_LOGE(TAG, "Failed to write");
    }
    return length;
}

int tcp_send_data(char *data, int32_t len);
static void tcp_send_task(void *pvParameters)
{
    int ret = 0;
    struct buf_el *el = NULL;
    struct buf_el *tmp = NULL;

    ESP_LOGI(TAG, "into send task");
    while(1) {
        if (wifi.status != WIFI_CONNECTED) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            //ESP_LOGI(TAG, "send task, not connected status");
            continue;
        }

        DL_FOREACH_SAFE(h_list, el, tmp) {
            int i = 0;
            char prt_buf[256] = {0};
            char *p = el->buf;
            for(i = 0; i < el->len; i++)
                sprintf(prt_buf+i*3, " %02X", *(p+i));
            ESP_LOGI("TP", "%s", prt_buf);
            do {
                ret = send(wifi.handle, el->buf, el->len, MSG_DONTWAIT);
                if (ret < 0) {
                    wifi.retry_num++;
                    ESP_LOGE(TAG, "send err, for each retry");
                } else {
                    // wait server recv ack
                    ESP_LOGI(TAG, "msg has been sent, wait for server ack");
                    if (xSemaphoreTake(xSemaphore, 2000 / portTICK_PERIOD_MS) == pdTRUE) {
                        wifi.retry_num = 0;
                        break;
                    } else {
                        wifi.retry_num++;
                    }
                }
                vTaskDelay(200 / portTICK_PERIOD_MS);
            } while(wifi.retry_num < 4);

            if (wifi.retry_num > 0) {
                wifi.status = WIFI_DISCONNECT;
                ESP_LOGE(TAG, "too more retry %d, skip this data", wifi.retry_num);
                wifi.retry_num = 0;
            }

            if (el->buf) {
                free(el->buf);
            }
            DL_DELETE(h_list, el);
            free(el);
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

int tcp_send_data(char *data, int32_t len)
{
    // send to host, flag MSG_DONTWAIT
    int i;
    char prt_buf[256];
    struct buf_el *el = NULL;
    char *p = data;

    ESP_LOGI(TAG, "APPEND data");
    for(i = 0; i < len; i++)
        sprintf(prt_buf+i*3, " %02X", *(p+i));
    ESP_LOGI("UR", "%s", prt_buf);

    el = (struct buf_el *)malloc(sizeof(struct buf_el));
    el->len = len;
    el->buf = malloc(len);
    memcpy(el->buf, data, el->len);
    DL_APPEND(h_list, el);

    return 0;
}

int tcp_close_socket(void)
{
    //shutdown(wifi.handle, 0);
    if (wifi.handle)
        close(wifi.handle);
    return 0;
}

static int tcp_socket_creat(char *host_ip, uint32_t port)
{
    int err = -1;
    int addr_family = 0;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = inet_addr(host_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;

    do {
        wifi.handle =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (wifi.handle < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }

        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, port);
        err = connect(wifi.handle, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            break;
        }

        ESP_LOGI(TAG, "Successfully connected");
        wifi.status = WIFI_CONNECTED;
    } while(0);
    
    return err;
}

static void tcp_recv_task(void *pvParameters)
{
    struct store_para *net_para = pvParameters;
    uint8_t rx_buffer[BUFFER_RX_MAX_LEN] = {0};
    uint8_t indx_buffer[BUFFER_INDEX_MAX_LEN] = {0};
    uint8_t out_buffer[BUFFER_OUT_CMD_LEN] = {0};
    uint8_t *prt_buffer = NULL;
    int i = 0, len;

    ESP_LOGI(TAG, "into recv task");
    while(1) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        if (wifi.status != WIFI_CONNECTED) {
            continue;
        }

        len = recv(wifi.handle, rx_buffer, sizeof(rx_buffer) - 1, 0);
        // Error occurred during receiving
        if (len < 0) {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            wifi.status = WIFI_DISCONNECT;
            continue;
        } else {
            // Data received
            xSemaphoreGive(xSemaphore);
            rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
            ESP_LOGI(TAG, "Received %d bytes from %s:", len, net_para->ip);
            ESP_LOGI(TAG, "%s", rx_buffer);
            if (len == 0)
                continue;

            memset((void *)out_buffer, 0, BUFFER_OUT_CMD_LEN);
            memset((void *)indx_buffer, 0, 32);
            sprintf((char *)indx_buffer, "+IND=RNET,%d,", len);
            memcpy((void *)out_buffer, indx_buffer, strlen((char *)indx_buffer));
            memcpy((void *)out_buffer + strlen((char *)indx_buffer), rx_buffer, len);
            len += strlen((char *)indx_buffer);

            prt_buffer = malloc(BUFFER_OUT_CMD_LEN*2);
            for(i = 0; i < len; i++)
                sprintf((char *)prt_buffer+i*3, " %02X", out_buffer[i]);
            ESP_LOGI("UW", "%s", prt_buffer);
            free(prt_buffer);
            esp_at_rhzl_write_data(out_buffer, len);
        }
    }
    vTaskDelete(NULL);
}

TaskHandle_t taskhandle_send;
TaskHandle_t taskhandle_recv;

static void tcp_client_task(void *pvParameters)
{
    int ret = -1;
    struct store_para net_para = {0};
    uint8_t out_buffer[BUFFER_OUT_CMD_LEN] = {0};
    xSemaphore = xSemaphoreCreateBinary();

    nvs_read_data_from_flash(&net_para);
    ESP_LOGI(TAG, "task net: %s, port %d\n", net_para.ip, net_para.port);
    if (!taskhandle_send)
        xTaskCreate(tcp_send_task, "tcp_send", 2048, &net_para, 8, &taskhandle_send);
    if (!taskhandle_recv)
        xTaskCreate(tcp_recv_task, "tcp_recv", 2048, &net_para, 7, &taskhandle_recv);

    while (1) {
        switch(wifi.status) {
            case WIFI_UNKNOW:
            case WIFI_READY:
                if (tcp_socket_creat((char *)&net_para.ip, net_para.port) < 0) {
                    ESP_LOGE(TAG, "creat socket failed %d, retry", ret);
                } else {
                    wifi.status = WIFI_CONNECTED;
                    sprintf((char *)out_buffer, "+IND=TCPC,%s,%d\r\n", net_para.ip, net_para.port);
                    esp_at_rhzl_write_data(out_buffer, strlen((char *)out_buffer));
                }
                break;
            case WIFI_DISCONNECT:
                tcp_close_socket();
                esp_at_rhzl_write_data((uint8_t *)"+IND=TCPD\r\n", strlen("+IND=TCPD\r\n"));
                wifi.status = WIFI_READY;
                break;
            case WIFI_CONNECTED:
                break;
            case WIFI_RETRY:
            default:
                ESP_LOGE(TAG, "ERROR wifi status");
                break;
        }

        if (wifi.handle < 0) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(wifi.handle, 0);
            close(wifi.handle);
            wifi.status = WIFI_UNKNOW;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void ap_record_sort_by_rssi(wifi_ap_record_t *ap_record_array, int len)
{
    int i, j;
    wifi_ap_record_t tmp_ap_record;
    int flag;
    for (i = 0; i < len - 1; i++) {
        flag = 1;
        for (j = 0; j < len - i - 1; j++) {
            if (ap_record_array[j].rssi < ap_record_array[j + 1].rssi) {
                tmp_ap_record = ap_record_array[j];
                ap_record_array[j] = ap_record_array[j + 1];
                ap_record_array[j + 1] = tmp_ap_record;
                flag = 0;
            }
        }
        if (flag == 1) {
            break;
        }  
    }
}

TaskHandle_t socket_open(struct store_para *para)
{
    TaskHandle_t taskhandle;
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    //ESP_ERROR_CHECK(example_connect());
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, &taskhandle);

    return 0;
}

int socket_close(TaskHandle_t *taskhandle)
{
    if (taskhandle) {
        //vTaskDelete(taskhandle_recv);
        //vTaskDelete(taskhandle_send);
        vTaskDelete(taskhandle);
    }
    return 0;
}
