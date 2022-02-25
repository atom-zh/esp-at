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

#define BUFFER_RX_MAX_LEN           256
#define BUFFER_INDEX_MAX_LEN        32
#define BUFFER_OUT_CMD_LEN          (BUFFER_RX_MAX_LEN + BUFFER_INDEX_MAX_LEN)

static const char *TAG = "wifi";

int socket_handle = 0;

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

static void tcp_heartbeat_task(void *pvParameters)
{
    while(1) {

        // send to mcu
        //esp_at_rhzl_write_data((uint8_t *)payload, strlen(payload));

        vTaskDelay(20000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

int tcp_close_socket(void)
{
    shutdown(socket_handle, 0);
    close(socket_handle);
    return 0;
}

int tcp_send_data(char *data, int32_t len)
{
    // send to host, flag MSG_DONTWAIT
    int err = send(socket_handle, data, len, 0);
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        tcp_close_socket();
        return -1;
    }
    return 0;
}

static void tcp_client_task(void *pvParameters)
{
    uint8_t rx_buffer[BUFFER_RX_MAX_LEN] = {0};
    uint8_t indx_buffer[BUFFER_INDEX_MAX_LEN] = {0};
    uint8_t out_buffer[BUFFER_OUT_CMD_LEN] = {0};
    static char host_ip[16] = {0};
    static int32_t port;
    int addr_family = 0;
    int ip_protocol = 0;
    int i = 0;
    net_para *net = pvParameters;

    //tcp_close_socket();

    strcpy(host_ip, (void *)net->ip);
    port = net->port;

    ESP_LOGI(TAG, "task net: %s, port %d\n", net->ip, net->port);
    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(host_ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(port);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;

        socket_handle =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (socket_handle < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, port);

        int err = connect(socket_handle, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            break;
        }

        ESP_LOGI(TAG, "Successfully connected");
        sprintf((char *)out_buffer, "+IND=TCPC,%s,%d\r\n", host_ip, port);
        esp_at_rhzl_write_data(out_buffer, strlen((char *)out_buffer));

        while (1) {
            xTaskCreate(tcp_heartbeat_task, "tcp_heartbeat", 2048, NULL, 6, NULL);
            int len = recv(socket_handle, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                esp_at_rhzl_write_data((uint8_t *)"+IND=TCPD\r\n", strlen("+IND=TCPD\r\n"));
                break;
            } else {
                // Data received
                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
                ESP_LOGI(TAG, "%s", rx_buffer);
                if (len == 0)
                    continue;

                memset((void *)out_buffer, 0, BUFFER_OUT_CMD_LEN);
                memset((void *)indx_buffer, 0, 32);
                sprintf((char *)indx_buffer, "+IND=RNET,%d,", len);
                memcpy((void *)out_buffer, indx_buffer, strlen((char *)indx_buffer));
                memcpy((void *)out_buffer + strlen((char *)indx_buffer), rx_buffer, len);
                len += strlen((char *)indx_buffer);
                for(i = 0; i < len; i++)
                    ESP_LOGI("O Data:", "%02X", out_buffer[i]);
                esp_at_rhzl_write_data(out_buffer, len);
            }
        }

        if (socket_handle != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(socket_handle, 0);
            close(socket_handle);
        }
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

int socket_open(net_para *net)
{
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    //ESP_ERROR_CHECK(example_connect());
    xTaskCreate(tcp_client_task, "tcp_client", 4096, net, 5, NULL);

    return 0;
}
