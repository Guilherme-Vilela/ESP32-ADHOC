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
    COMPLETE,
    SEND_1,
    SEND_2
} communication_stages;
typedef struct current_connections
{
    uint8_t mac_adress[6];
    openmac_sta_state_t status;
    uint8_t communication_attempts;
    uint16_t wait_time_to_send;
    uint64_t last_transmission;
    communication_stages communication_state;
    struct current_connections *next;
} current_connections;

typedef struct payload_mensages
{
    uint8_t id;                     //0
    //uint64_t time_create;            
    uint8_t  destination_mac[6];    //1-6
    uint8_t size_of_data; //        //7
    uint8_t size_of_path; //        //8
    uint8_t size_packet;           //9
    uint8_t *data;                  //11 - 
    uint8_t *path_mensage; // vai virar uma nova estrutura       MAC-TIME/MAC-TIME
    struct payload_mensages *next;

} payload_mensages;

//0.000001
#define time_transmit 1000000 // 1s
#define max_attempts 10 // 10 tentativas
#define wait_time_to_send_dafault 10000 //tempo de envio das mensagens, definiado pelo time_unit
#define wait_time_to_send_probe_request 5000 //tempo de envio das mensagens, definiado pelo time_unit
#define size_mensage 100
#define time_unit 1000 // tempo definiado como 1ms. 1 = 1us
#define NOME_ESP "Ola sou NO 2"
#define ID_ESP 4


void open_mac_rx_callback(wifi_promiscuous_pkt_t* packet);
void open_mac_tx_func_callback(tx_func* t);

void mac_task(void* pvParameters);

esp_err_t openmac_netif_start();
