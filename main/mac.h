#pragma once
#include <stdint.h>

typedef enum
{
    EVENT_TX_REQUEST,   // App pede envio
    EVENT_TX_COMPLETE,  // Hardware confirma envio
    EVENT_TX_HELLO
} mac_event_type_t;

typedef struct
{
    mac_event_type_t type;

    union
    {
        // Dados para ENVIO (Vem da App)
        struct
        {
            uint8_t dest_mac[6];
            uint8_t *payload;
            uint16_t len;
        } tx_req;

        // Dados de RETORNO (Vem do Hardware)
        struct
        {
            uint16_t seq_num;
            bool success; // true = ACK, false = Fail
        } tx_conf;
    };
} mac_event_t;

typedef enum
{
    // Tipos geranciados pelo MAC Layer
    OPPNET_LINK_DATA = 0x01, // Contém carga útil da aplicação (Hello, Msg, etc)
    OPPNET_LINK_ACK = 0x06   // Confirmação física de recebimento
} mac_packet_type_t;
typedef struct
{
    uint16_t len;
    int8_t rssi;
    uint8_t *buffer; // Ponteiro para memória alocada dinamicamente
} mac_rx_item_t;

typedef struct
{
    uint8_t dest_mac[6];
    uint8_t *payload;
    uint16_t len;
} mac_tx_request_t;

typedef struct
{
    uint8_t mac[6];
    uint16_t last_seq_ctrl;
} neighbor_cache_t;



// esp_err_t openmac_netif_start();
typedef struct
{
    uint8_t type; // 0x01 = HELLO
    // Futuro: Adicionar carga de bateria, vizinhos conhecidos, etc.
} oppnet_hello_payload_t;


void open_mac_rx_callback(wifi_promiscuous_pkt_t *packet);
void open_mac_tx_func_callback(tx_func *t);

void mac_init();
void mac_send_package(void *pvParameters);

extern QueueHandle_t mac_event_queue;