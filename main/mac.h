#include "hardware.h"
typedef enum {
    PROBE_RESPONSE,                             
    AUTHENTICATION_REQUEST,         
    AUTHENTICATION_RESPONSE,            
    ASSOCIATION_REQUEST,            
    ASSOCIATION_RESPONSE,           
    CONNECTED          
} openmac_sta_state_t;

typedef enum {
    WAIT,
    SEND_MESSAGE_DESTINATIONS,
    SEND_MESSAGE_DESTINATIONS_INTEREST,  
    SEND_REQUESTED_MESSAGES_INTEREST,
    SEND_REQUESTED_MESSAGES,
    COMPLETE

} communication_stages;

// typedef enum {
//     SEND_DATA_DESTINATIONS = 0,
//     SEND_DATA_NAME =1
// } dtn_comunicate_state;

//0.000001
#define time_transmit 1000000 // 1s
#define max_attempts 10 // 10 tentativas
#define wait_time_to_send_dafault 1000 //tempo de envio das mensagens, definiado pelo time_unit
#define wait_time_to_send_probe_request 5000 //tempo de envio das mensagens, definiado pelo time_unit
#define size_mensage 100
#define time_unit 1 // tempo definiado com 1ms. 1 = 1us
#define NOMEESP "Ola sou NO 3"


void open_mac_rx_callback(wifi_promiscuous_pkt_t* packet);
void open_mac_tx_func_callback(tx_func* t);

void mac_task(void* pvParameters);

esp_err_t openmac_netif_start();
