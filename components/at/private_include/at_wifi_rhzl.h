#ifndef __AT_WIFI_RHZL_H__
#define __AT_WIFI_RHZL_H__

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PWD_MAX_LEN  64
#define WIFI_IP_MAX_LEN   16

enum WIFI_MODE {
    WLMODE_UNKNOW = -1,
    WLMODE_STD = 0,
    WLMODE_RHZD,
    WLMODE_MAX,
};

struct store_para{
    uint8_t ssid[WIFI_SSID_MAX_LEN];      /**< SSID of target AP. */
    uint8_t password[WIFI_PWD_MAX_LEN];  /**< Password of target AP. */
    uint8_t ip[WIFI_IP_MAX_LEN];
    int32_t port;
    enum WIFI_MODE mode;
};

TaskHandle_t socket_open(struct store_para *para);
int socket_close(TaskHandle_t *taskhandle);
void ap_record_sort_by_rssi(wifi_ap_record_t *ap_record_array, int len);
int nvs_write_data_to_flash(struct store_para *para);
int nvs_read_data_from_flash(struct store_para *para);

#endif