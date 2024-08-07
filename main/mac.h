#include "hardware.h"
typedef enum {
    PROBE_RESPONSE,
    AUTHENTICATION_REQUEST,
    AUTHENTICATION_RESPONSE,
    ASSOCIATION_REQUEST,
    ASSOCIATION_RESPONSE,
    CONNECTED,
    SEND_DATA
} openmac_sta_state_t;


#define time_transmit 1000000 //
#define time_out 10000000 // time for desconection
#define pre_escale_probe_request 5 // 0 a 255 
#define time_unit 1 ///definida com 1u / time_unit
void open_mac_rx_callback(wifi_promiscuous_pkt_t* packet);
void open_mac_tx_func_callback(tx_func* t);

void mac_task(void* pvParameters);

esp_err_t openmac_netif_start();
