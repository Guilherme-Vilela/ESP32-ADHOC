#include "esp_wifi.h" // included for the definition of the wifi_promiscuous_pkt_t struct
#include "esp_mac.h"  // included for the MAC2STR macro
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_random.h"

#include "hardware.h"
#include "80211.h"
#include "mac.h"
#include "proprietary.h"

#include "driver/gpio.h"
#include <string.h>

static char *TAG = "mac.c";
static tx_func *tx = NULL;
static QueueHandle_t reception_queue = NULL;
macaddr_t ssid = {'w', 'i', 'f', 'd', 't', 'n'};
macaddr_t ap_address = {0, 0, 0, 0, 0, 0};

current_connections *list_connections = NULL;
payload_mensages *list_mensages = NULL;
uint8_t mac_frame_header_template[] = {
    IEEE80211_FRAME_CONTROL,    // #define IEEE80211_FRAME_CONTROL     0x00, 0x00
    IEEE80211_DURATION_ID,      // #define IEEE80211_DURATION_ID       0x00, 0x00
    IEEE80211_RECIVER_ADDR,     // #define IEEE80211_RECIVER_ADDR      0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    IEEE80211_TRANSMITTER_ADDR, // #define IEEE80211_TRANSMITTER_ADDR  0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    IEEE80211_BSSID,            // #define IEEE80211_BSSID             0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    IEEE80211_SEQUENCE_CONTROL, // #define IEEE80211_SEQUENCE_CONTROL  0x00, 0x00
};

current_connections *create_list_current_connections()
{
    return NULL;
}
payload_mensages *create_list_payload_mensages()
{
    return NULL;
}

current_connections *insert_current_connections(current_connections *head, uint8_t mac[6], uint8_t state)
{
    current_connections *new_conection = malloc(sizeof(current_connections));
    current_connections *item = head;

    memcpy(new_conection->mac_adress, mac, 6);
    new_conection->status = state;
    new_conection->next = NULL;
    new_conection->communication_attempts = 0;
    new_conection->last_transmission = 0;
    new_conection->communication_state = WAIT;
    while (new_conection->wait_time_to_send > 750 || new_conection->wait_time_to_send < 500)
    {
        esp_fill_random(&new_conection->wait_time_to_send, 2);
    }

    if (head == NULL)
    {
        return new_conection;
    }

    while (item->next != NULL)
    {
        item = item->next;
    }
    item->next = new_conection;

    return head;
}
payload_mensages *create_payload_mensage(payload_mensages *head, uint8_t mac[6], uint8_t *mensage_to_send, uint8_t size_of_mensage)
{
    payload_mensages *new_mensage = malloc(sizeof(payload_mensages));
    payload_mensages *item = head;
    uint8_t *path = malloc(7);
    memcpy(path, module_mac_addr, 6);
    path[7] = '/';

    new_mensage->id = 0x65;
    // new_mensage->time_create = esp_timer_get_time();
    memcpy(new_mensage->destination_mac, mac, 6);
    new_mensage->size_of_data = size_of_mensage; // size max 1500 bytes -mac frame header
    new_mensage->size_of_path = 7;

    new_mensage->data = mensage_to_send;

    new_mensage->path_mensage = path; // vai virar uma nova estrutura       MAC1/MAC2
    new_mensage->next = NULL;
    new_mensage->size_packet = new_mensage->size_of_data + new_mensage->size_of_path + 10;
    if (head == NULL)
    {
        return new_mensage;
    }

    while (item->next != NULL)
    {
        item = item->next;
    }
    item->next = new_mensage;

    return head;
}
payload_mensages *insert_payload_mensage(payload_mensages *head, uint8_t *mensage_received)
{
    payload_mensages *new_mensage = malloc(sizeof(payload_mensages));
    payload_mensages *item = head;

    memcpy(&new_mensage->id, mensage_received, 1);
    // memcpy(&new_mensage->time_create, 1 + mensage_received, 8);
    memcpy(new_mensage->destination_mac, 1 + mensage_received, 6);
    memcpy(&new_mensage->size_of_data, 7 + mensage_received, 1);
    memcpy(&new_mensage->size_of_path, 8 + mensage_received, 1);
    // memcpy(&new_mensage->size_packet, 9 + mensage_received, 1);
    new_mensage->size_packet = 0x41;
    ESP_LOGW(TAG, "taamnho mensagem : %x", new_mensage->size_of_data);
    ESP_LOGW(TAG, "caminho : %x", new_mensage->size_of_path);
    ESP_LOGW(TAG, "tamanho pacote : %x", new_mensage->size_packet);
    uint8_t *mensage_data_received = malloc(new_mensage->size_of_data);
    uint8_t *mensage_path_received = malloc(new_mensage->size_of_path);

    memcpy(mensage_data_received, 10 + mensage_received, new_mensage->size_of_data);
    memcpy(mensage_path_received, (10 + new_mensage->size_of_data) + mensage_received, new_mensage->size_of_path);

    new_mensage->data = mensage_data_received;
    new_mensage->path_mensage = mensage_data_received;
    new_mensage->next = NULL;

    if (head == NULL)
    {
        return new_mensage;
    }

    while (item->next != NULL)
    {
        item = item->next;
    }
    item->next = new_mensage;

    return head;
}
payload_mensages *remove_mensage(payload_mensages **head, payload_mensages **connection_removed)
{
    payload_mensages *item = *head;
    payload_mensages *previous_item = NULL;
    ESP_LOGD(TAG, "APAGANDO A CONEXÃO");
    while (item != NULL && item != *connection_removed)
    {
        previous_item = item;
        item = item->next;
    }
    free(item->data);
    free(item->path_mensage);
    if (item != NULL)
    {
        if (previous_item == NULL)
        {
            *head = item->next;
            previous_item = item->next;
        }
        else
        {
            previous_item->next = item->next;
        }
        free(item);
    }
    return previous_item;
}
void update_conection(current_connections *connection, uint8_t state)
{
    connection->status = state;
    connection->communication_attempts = 0;
}
void update_communication_state(current_connections *connection, communication_stages state)
{
    connection->communication_state = state;
    connection->communication_attempts = 0;
}
void update_wait_time_to_send(current_connections *connection)
{
    while (connection->wait_time_to_send > 750 || connection->wait_time_to_send < 500)
    {
        esp_fill_random(&connection->wait_time_to_send, 2);
    }
}
current_connections *search_conection(current_connections *head, uint8_t mac[6])
{
    current_connections *item = head;
    while (item != NULL && memcmp(item->mac_adress, mac, 6))
    {
        item = item->next;
    }
    return item;
}

current_connections *remove_conection(current_connections **head, current_connections **connection_removed)
{
    current_connections *item = *head;
    current_connections *previous_item = NULL;
    ESP_LOGD(TAG, "APAGANDO A CONEXÃO");
    while (item != NULL && item != *connection_removed)
    {
        previous_item = item;
        item = item->next;
    }

    if (item != NULL)
    {
        if (previous_item == NULL)
        {
            *head = item->next;
            previous_item = item->next;
        }
        else
        {
            previous_item->next = item->next;
        }
        free(item);
    }
    return previous_item;
}

int search_mac(current_connections *head, uint8_t mac[6])
{
    current_connections *item = head;
    if (item != NULL)
    {
        while (memcmp(item->mac_adress, mac, 6) && item != NULL)
        {
            item = item->next;
        }
        if (item != NULL)
        {
            return true;
        }
    }
    return false;
}
void build_frame(uint8_t *frame, uint8_t *ra, uint8_t *ta, uint8_t *bssid)
{
    // set receiver address
    memcpy(&frame[4], ra, 6);
    // set transmitter address
    memcpy(&frame[10], ta, 6);
    // set bssid
    memcpy(&frame[16], bssid, 6);
}

void set_addresses(uint8_t *frame, uint8_t first_byte_frame_control, uint8_t *ra)
{
    // set first_byte frame control
    frame[0] = first_byte_frame_control;

    // set receiver address
    memcpy(&frame[4], ra, 6);
}

void open_mac_rx_callback(wifi_promiscuous_pkt_t *packet)
{
    // Gets called with a packet that was received. This function does not need to free the memory of the packet,
    //  but the packet will become invalid after this function returns. If you need any data from the packet,
    //  better copy it before returning!
    // Please avoid doing heavy processing here: it's not in an interrupt, but if this function is not fast enough,
    // the RX queue that is used to pass packets to this function might overflow and drop packets.

    mac80211_frame *p = (mac80211_frame *)packet->payload;
    uint8_t mac_listen_probe_request[6] = MAC_PROBE_REQUEST;
    // printf("\n DADO RECEBIDO :");
    // for (int contador = 0; contador < 24; contador++)
    // {
    //     printf(" %x ", packet->payload[contador]);
    // }
    // printf("\n\n");

    // uint8_t value = packet->payload[0];
    // p->frame_control.sub_type = value & 0b00001111;
    // p->frame_control.type = ((value & 0b00110000) >> 4);
    // p->frame_control.protocol_version = ((value & 0b11000000) >> 6);
    // ESP_LOG_BUFFER_HEXDUMP("netif-rx 802.11  ", packet->payload, packet->rx_ctrl.sig_len - 4, ESP_LOG_INFO);

    // ESP_LOGW(TAG, "TIPO : %x", p->frame_control.type);
    // ESP_LOGW(TAG, "SUBTIPO : %x", p->frame_control.sub_type);
    // ESP_LOGW(TAG, "TAMANHO DA MENSAGEM : %i", packet->rx_ctrl.sig_len);
    //  check that receiver mac address matches our mac address or is broadcast
    if ((memcmp(module_mac_addr, p->receiver_address, 6)) && (memcmp(mac_listen_probe_request, p->receiver_address, 6)))
    { //&& (memcmp(BROADCAST_MAC, p->receiver_address, 6))
        // We're not interested in this packet, return early to avoid having to copy it further to the networking stack
        ESP_LOGD(TAG, "Discarding packet from " MACSTR " to " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
        return;
    }
    // verificação de probe request
    // ESP_LOGI(TAG, "Accepted: from " MACSTR " to " MACSTR " type=%d, subtype=%d from_ds=%d to_ds=%d", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address), p->frame_control.type, p->frame_control.sub_type, p->frame_control.from_ds, p->frame_control.to_ds);

    if (!reception_queue)
    {
        ESP_LOGI(TAG, "Received, but queue does not exist yet");
        return;
    }
    // 28 is size of rx_ctrl, 4 is size of FCS (which we don't need)
    wifi_promiscuous_pkt_t *packet_queue_copy = malloc(packet->rx_ctrl.sig_len + 24);
    memcpy(packet_queue_copy, packet, packet->rx_ctrl.sig_len + 24);
    if (!(xQueueSendToBack(reception_queue, &packet_queue_copy, 0)))
    {
        ESP_LOGW(TAG, "MAC RX queue full!");
    }
}

void open_mac_tx_func_callback(tx_func *t)
{
    // This function will get called exactly once, with as argument a function (`bool tx_func(uint8_t* packet, uint32_t len)`).
    // The function that is passed will TX packets. If it returned `true`, that means that the packet was sent. If false,
    //  you'll need to call the function again.
    tx = t;
}

void send_mensages(current_connections *connection)
{
    // 0
    // 0x31 dados
    // 0x32 confirmacao
    // 0x33 esperando
    // 0x34 sem dados
    uint8_t mac_frame_data[30];
    switch (connection->communication_state)
    {

    case SEND_MESSAGE_DESTINATIONS:
        if (list_mensages != NULL)
        {
            ESP_LOGW(TAG, "enviadno mensagem");
            connection->communication_state = SEND_MESSAGE_DESTINATIONS;

            uint16_t size_packet_to_send = 29 + list_mensages->size_packet;
            uint16_t size_mensage_and_header = 35 + list_mensages->size_of_data;
            uint8_t mac_frame_data2[size_packet_to_send];

            memcpy(mac_frame_data2, mac_frame_header_template, 24);
            mac_frame_data2[24] = 0x31;

            memcpy(&mac_frame_data2[25], list_mensages, 10);
            memcpy(&mac_frame_data2[35], list_mensages->data, list_mensages->size_of_data);
            memcpy(&mac_frame_data2[size_mensage_and_header], list_mensages->path_mensage, list_mensages->size_of_path);

            for (int i = 0; i < 2; i++)
            {
                mac_frame_data2[size_packet_to_send - i] = 0xFF;
            }

            set_addresses(mac_frame_data2, IEEE80211_ACK, connection->mac_adress);
            tx(mac_frame_data2, sizeof(mac_frame_data2));
        }
        else
        {
            memcpy(mac_frame_data, mac_frame_header_template, 24);
            mac_frame_data[24] = 0x34;

            for (int i = 0; i < 4; i++)
            {
                mac_frame_data[28 - i] = 0xFF;
            }

            set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
            tx(mac_frame_data, sizeof(mac_frame_data));
        }
        break;

    case COMPLETE:

        memcpy(mac_frame_data, mac_frame_header_template, 24);
        mac_frame_data[24] = 0x32;

        for (int i = 0; i < 4; i++)
        {
            mac_frame_data[28 - i] = 0xFF;
        }

        set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
        tx(mac_frame_data, sizeof(mac_frame_data));
        connection->wait_time_to_send = 5000;
        break;

    default:
        memcpy(mac_frame_data, mac_frame_header_template, 24);
        mac_frame_data[24] = 0x33;

        for (int i = 0; i < 4; i++)
        {
            mac_frame_data[28 - i] = 0xFF;
        }

        set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
        tx(mac_frame_data, sizeof(mac_frame_data));
        break;
    }
}
void send_probe_request(void *pvParameters){
    while (true) {
        
        if (!tx)
        {
            ESP_LOGW(TAG, "no transmit function yet");
         }else{
            //esp_fill_random(&time_wait_probe_requenst, 2);
           // time_wait_probe_requenst = time_wait_probe_requenst & 0x0FFF;
            // time_wait_probe_requenst += wait_time_to_send_probe_request;
            //  enviando probe request
            ESP_LOGI(TAG, "Sending PROBEREQUEST");
            set_addresses(mac_frame_header_template, IEEE80211_PROBE_REQUEST, ap_address);
            tx(mac_frame_header_template, sizeof(mac_frame_header_template));
            //last_transmission_probe_request = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}
void send_mensage_mac_protocol(current_connections *connection)
{
    connection->communication_attempts++;
    switch (connection->status)
    {
    case PROBE_RESPONSE:
        ESP_LOGI(TAG, "Sending PROBE RESPONSE");
        set_addresses(mac_frame_header_template, IEEE80211_PROBE_RESPONSE, connection->mac_adress);
        tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;
    case AUTHENTICATION_REQUEST:
        ESP_LOGI(TAG, "Sending authentication Request!");
        set_addresses(mac_frame_header_template, IEEE80211_AUTHENTICATION, connection->mac_adress);
        tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;
    case AUTHENTICATION_RESPONSE:
        ESP_LOGI(TAG, "Sending authentication response!");
        set_addresses(mac_frame_header_template, IEEE80211_AUTHENTICATION, connection->mac_adress);
        tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;
    case ASSOCIATION_REQUEST:
        ESP_LOGI(TAG, "Sending association request frame!");
        set_addresses(mac_frame_header_template, IEEE80211_ASSOCIATION_REQ, connection->mac_adress);
        tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;
    case ASSOCIATION_RESPONSE:
        ESP_LOGI(TAG, "Sending association response frame!");
        set_addresses(mac_frame_header_template, IEEE80211_ASSOCIATION_RESP, connection->mac_adress);
        tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;
    case CONNECTED:
        ESP_LOGI(TAG, "ACK !");
        send_mensages(connection);
        // set_addresses(mac_frame_header_template, IEEE80211_ACK, connection->mac_adress);
        // tx(mac_frame_header_template, sizeof(mac_frame_header_template));
        break;

    default:
        break;
    }
}
void mac_task(void *pvParameters)
{
    
    //uint64_t last_transmission_probe_request = esp_timer_get_time();
    uint16_t time_wait_probe_requenst = wait_time_to_send_probe_request;
    list_connections = create_list_current_connections();
    list_mensages = create_list_payload_mensages();
    //if (ID_ESP == 1)
    //{
        //uint8_t mac_target[6] = {0Xff, 0x22, 0x33, 0x71, 0x45, 0x90};
       // uint8_t mensage_aux[35] = "ACIDENTE NO 1, POSICAO X Y, ATENCAO";
        //uint8_t *mensage_to_send = malloc(35);
        //memcpy(mensage_to_send, mensage_aux, 35);

        //list_mensages = create_payload_mensage(list_mensages, mac_target, mensage_to_send, 35);
       // gpio_set_level(19, 1);
        
    //}//
    current_connections *connection_aux = NULL;

    build_frame(mac_frame_header_template, ap_address, module_mac_addr, ssid);

    ESP_LOGI(TAG, "Starting mac_task, running on %d", xPortGetCoreID());

    reception_queue = xQueueCreate(10, sizeof(wifi_promiscuous_pkt_t *));
    assert(reception_queue);

    while (true)
    {
        wifi_promiscuous_pkt_t *packet;
        // TRATAMENTO DE MENSAGEM
        if (xQueueReceive(reception_queue, &packet, 10))
        {
            mac80211_frame *p = (mac80211_frame *)packet->payload;
            uint8_t value = packet->payload[0];

            ESP_LOG_BUFFER_HEXDUMP("netif-rx 802.11 DADO RECEBIDO ", packet->payload, packet->rx_ctrl.sig_len, ESP_LOG_INFO);

            p->frame_control.sub_type = value & 0b00001111;
            p->frame_control.type = ((value & 0b00110000) >> 4);
            p->frame_control.protocol_version = ((value & 0b11000000) >> 6);

            connection_aux = search_conection(list_connections, p->transmitter_address);

            ESP_LOGW(TAG, "TIPO : %x", p->frame_control.type);
            ESP_LOGW(TAG, "SUBTIPO : %x", p->frame_control.sub_type);
            ESP_LOGW(TAG, "TAMANHO DA MENSAGEM : %i", packet->rx_ctrl.sig_len);
            ESP_LOGW(TAG, "DADO ENVIADO POR : " MACSTR "\n", MAC2STR(p->transmitter_address));

            // verifica se a conexão existe
            if (connection_aux == NULL)
            { // se o mac ainda nao esta na lsita de conexões
                if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ)
                {
                    list_connections = insert_current_connections(list_connections, p->transmitter_address, PROBE_RESPONSE);
                }
                else if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP)
                {
                    list_connections = insert_current_connections(list_connections, p->transmitter_address, AUTHENTICATION_REQUEST);
                }
            }
            else
            { // se o mac esta na lsita de conexões
                if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP && connection_aux->status == PROBE_RESPONSE)
                {
                    update_conection(connection_aux, AUTHENTICATION_REQUEST);
                    ESP_LOGI(TAG, "Passando para etapa de autenticação");
                }
            }

            connection_aux = search_conection(list_connections, p->transmitter_address);

            if (connection_aux != NULL)
            {
                if (p->frame_control.type == IEEE80211_TYPE_MGT)
                {
                    switch (connection_aux->status)
                    {
                    case PROBE_RESPONSE: // WAIT PACK AUTH REQUEST
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION)
                        {
                            ESP_LOGW(TAG, "Authentication received from=" MACSTR " to= " MACSTR "\n\n", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection_aux, AUTHENTICATION_RESPONSE);
                            send_mensage_mac_protocol(connection_aux); // RESPONDE AS SOLICITAÇÕES DE IMEDIADO //CUIDAR PARA NAO SOBRECARREGAR
                        }
                        break;
                    case AUTHENTICATION_REQUEST: // WAIT PACK AUTH RESPONSE
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION)
                        {
                            ESP_LOGW(TAG, "Authentication response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection_aux, ASSOCIATION_REQUEST);
                            send_mensage_mac_protocol(connection_aux); // RESPONDE AS SOLICITAÇÕES DE IMEDIADO //CUIDAR PARA NAO SOBRECARREGAR
                        }
                        break;
                    case AUTHENTICATION_RESPONSE: // authenticated, wait for association response packet
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ)
                        {
                            ESP_LOGW(TAG, "Association request received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection_aux, ASSOCIATION_RESPONSE);
                            send_mensage_mac_protocol(connection_aux); // RESPONDE AS SOLICITAÇÕES DE IMEDIADO //CUIDAR PARA NAO SOBRECARREGAR
                        }
                        break;
                    case ASSOCIATION_REQUEST: // associated
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP)
                        {
                            ESP_LOGW(TAG, "Association response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection_aux, CONNECTED);
                            update_communication_state(connection_aux, SEND_MESSAGE_DESTINATIONS);
                            send_mensage_mac_protocol(connection_aux); // RESPONDE AS SOLICITAÇÕES DE IMEDIADO //CUIDAR PARA NAO SOBRECARREGAR
                        }
                        break;
                    default:
                        break;
                    }
                }
                else if (p->frame_control.type == IEEE80211_TYPE_CTL)
                {
                    switch (connection_aux->status)
                    {
                    case CONNECTED: //
                        // ESP_LOGW(TAG, "COMANDO : %i \n", packet->payload[49]);
                        ESP_LOGW(TAG, "DADO  : %x \n", packet->payload[24]);
                        if (p->frame_control.sub_type == IEEE80211_TYPE_CTL_SUBTYPE_ACK)
                        {
                            update_conection(connection_aux, CONNECTED);
                            // 4 sem dados
                            // 1 dados
                            // 2 confirmacao
                            // 3 esperando
                            switch (packet->payload[24])
                            {
                            case 0x34:
                                if (connection_aux->communication_state == WAIT)
                                    update_communication_state(connection_aux, SEND_MESSAGE_DESTINATIONS);
                                else if (connection_aux->communication_state == SEND_MESSAGE_DESTINATIONS)
                                    update_communication_state(connection_aux, COMPLETE);
                                break;
                            case 0x31:
                                if (connection_aux->communication_state == WAIT)
                                {
                                    gpio_set_level(19, 1);
                                    update_communication_state(connection_aux, SEND_MESSAGE_DESTINATIONS);
                                    list_mensages = insert_payload_mensage(list_mensages, &packet->payload[25]);
                                    ESP_LOG_BUFFER_HEXDUMP("MENSAGEM MONTADA", list_mensages, list_mensages->size_packet, ESP_LOG_INFO);
                                }
                                // SALVA
                                else if (connection_aux->communication_state == SEND_MESSAGE_DESTINATIONS)
                                {
                                    gpio_set_level(19, 1);
                                    update_communication_state(connection_aux, COMPLETE);
                                    list_mensages = insert_payload_mensage(list_mensages, &packet->payload[25]);
                                    ESP_LOG_BUFFER_HEXDUMP("MENSAGEM MONTADA", list_mensages, list_mensages->size_packet, ESP_LOG_INFO);
                                    // SALVA
                                }
                                break;
                            case 0x32:
                                if (connection_aux->communication_state == SEND_MESSAGE_DESTINATIONS)
                                    update_communication_state(connection_aux, COMPLETE);
                                break;
                            default:
                                break;
                            }
                            // send_mensage_mac_protocol(connection_aux); // RESPONDE AS SOLICITAÇÕES DE IMEDIADO //CUIDAR PARA NAO SOBRECARREGAR
                        }
                        break;
                    case ASSOCIATION_RESPONSE: //
                        if (p->frame_control.sub_type == IEEE80211_TYPE_CTL_SUBTYPE_ACK)
                        {
                            update_conection(connection_aux, CONNECTED);
                        }
                        break;

                    default:
                        break;
                    }
                }
            }
        }

        connection_aux = list_connections;

        while (connection_aux != NULL)
        {

            if (((esp_timer_get_time() - connection_aux->last_transmission) / time_unit) > connection_aux->wait_time_to_send)
            {
                send_mensage_mac_protocol(connection_aux);
                connection_aux->last_transmission = esp_timer_get_time();
            }

            if (connection_aux->communication_attempts % 3 == 0)
            {
                update_wait_time_to_send(connection_aux);
            }

            if (connection_aux->communication_attempts > max_attempts)
            {
                connection_aux = remove_conection(&list_connections, &connection_aux);
            }

            if (connection_aux != NULL)
            {
                connection_aux = connection_aux->next;
            }
        }
    }
}

// void send_name(uint8_t *mac_reciver)
// {
//     uint8_t mac_frame_data[54];
//     char name[12] = NOME_ESP;

//     memcpy(mac_frame_data, mac_frame_header_template, 24);
//     mac_frame_data[24] = 0;
//     mac_frame_data[25] = 0;
//     mac_frame_data[27] = 0;
//     mac_frame_data[28] = 0;

//     memcpy(&mac_frame_data[29], name, 12);
//     for (int contador = 41; contador < 46; contador++)
//     {
//         mac_frame_data[contador] = 0;
//     }
//     set_addresses(mac_frame_data, IEEE80211_ACK, mac_reciver);
//     tx(mac_frame_data, sizeof(mac_frame_data));
// }