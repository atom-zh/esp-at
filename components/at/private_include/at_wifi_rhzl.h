#ifndef __AT_WIFI_RHZL_H__
#define __AT_WIFI_RHZL_H__

typedef struct {
    uint8_t ip[16];
    int32_t port;
}net_para;

int socket_open(net_para *net);
int tcp_send_data(char *data, int32_t len);
int tcp_close_socket(void);
void ap_record_sort_by_rssi(wifi_ap_record_t *ap_record_array, int len);

#endif