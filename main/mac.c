#include "esp_wifi.h" // included for the definition of the wifi_promiscuous_pkt_t struct
#include "esp_mac.h"  // included for the MAC2STR macro
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"

#include "hardware.h"
#include "80211.h"
#include "mac.h"
#include "proprietary.h"

#include <string.h>

static char *TAG = "mac.c";
static tx_func *tx = NULL;
static QueueHandle_t reception_queue = NULL;

macaddr_t ssid = {'w', 'i', 'f', 'd', 't', 'n'};
macaddr_t ap_address = {0, 0, 0, 0, 0, 0};

typedef struct current_connections
{
    uint8_t mac_adress[6];
    openmac_sta_state_t status;
    uint64_t last_communication_time;
    struct current_connections *next;
} current_connections;

current_connections *list_connections = NULL;

uint8_t mac_frame_header_template[] = {
    IEEE80211_FRAME_CONTROL,
    IEEE80211_DURATION_ID,
    IEEE80211_RECIVER_ADDR,
    IEEE80211_TRANSMITTER_ADDR,
    IEEE80211_BSSID,
    IEEE80211_SEQUENCE_CONTROL,
};

current_connections *create_list_current_connections()
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
    new_conection->last_communication_time = esp_timer_get_time() / time_unit;

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

void update_conection(current_connections *conection, uint8_t state)
{
    conection->status = state;
    conection->last_communication_time = esp_timer_get_time() / time_unit;
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

// Gets called with a packet that was received. This function does not need to free the memory of the packet,
//  but the packet will become invalid after this function returns. If you need any data from the packet,
//  better copy it before returning!
// Please avoid doing heavy processing here: it's not in an interrupt, but if this function is not fast enough,
// the RX queue that is used to pass packets to this function might overflow and drop packets.

void open_mac_rx_callback(wifi_promiscuous_pkt_t *packet)
{
    mac80211_frame *p = (mac80211_frame *)packet->payload;
    uint8_t mac_listen_probe_request[6] = MAC_PROBE_REQUEST;
    current_connections *connection_aux = NULL;
    printf("\n DADO RECEBIDO :");
    for (int contador = 0; contador < 24; contador++)
    {
        printf(" %x ", packet->payload[contador]);
    }
    printf("\n\n");
    if (packet->payload[0] < 16)
    {
        uint8_t valor = packet->payload[0];
        p->frame_control.sub_type = valor;
        p->frame_control.type = 0;
        p->frame_control.protocol_version = 0;
    }

    printf("Tipo   = %x \n", p->frame_control.type);
    printf("Subtipo= %x \n", p->frame_control.sub_type);
    //  check that receiver mac address matches our mac address or is broadcast
    if ((memcmp(module_mac_addr, p->receiver_address, 6)) && (memcmp(mac_listen_probe_request, p->receiver_address, 6)))
    { //&& (memcmp(BROADCAST_MAC, p->receiver_address, 6))
        // We're not interested in this packet, return early to avoid having to copy it further to the networking stack
        ESP_LOGD(TAG, "Discarding packet from " MACSTR " to " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
        return;
    }
    // verificação de probe request
    connection_aux = search_conection(list_connections, p->transmitter_address);

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

    // ESP_LOGI(TAG, "Accepted: from " MACSTR " to " MACSTR " type=%d, subtype=%d from_ds=%d to_ds=%d", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address), p->frame_control.type, p->frame_control.sub_type, p->frame_control.from_ds, p->frame_control.to_ds);

    if (!reception_queue)
    {
        ESP_LOGI(TAG, "Received, but queue does not exist yet");
        return;
    }
    // 28 is size of rx_ctrl, 4 is size of FCS (which we don't need)
    wifi_promiscuous_pkt_t *packet_queue_copy = malloc(packet->rx_ctrl.sig_len + 28 - 4);
    memcpy(packet_queue_copy, packet, packet->rx_ctrl.sig_len + 28 - 4);
    if (!(xQueueSendToBack(reception_queue, &packet_queue_copy, 0)))
    {
        ESP_LOGW(TAG, "MAC RX queue full!");
    }
}

// This function will get called exactly once, with as argument a function (`bool tx_func(uint8_t* packet, uint32_t len)`).
// The function that is passed will TX packets. If it returned `true`, that means that the packet was sent. If false,
//  you'll need to call the function again.
void open_mac_tx_func_callback(tx_func *t)
{
    tx = t;
}

void send_mensage()
{
    // char8_t mac_frame_data[1500];

    // busca a mensagem
    // trata a mensagem
    // manda mensagem
    //
}

void mac_task(void *pvParameters)
{
    uint8_t temp_probe_request = 0;
    uint64_t last_transmission_us = esp_timer_get_time();
    list_connections = create_list_current_connections();
    current_connections *connection = NULL;

    build_frame(mac_frame_header_template, ap_address, module_mac_addr, ssid);

    ESP_LOGI(TAG, "Starting mac_task, running on %d", xPortGetCoreID());

    reception_queue = xQueueCreate(10, sizeof(wifi_promiscuous_pkt_t *));
    assert(reception_queue);

    while (true)
    {
        wifi_promiscuous_pkt_t *packet;
        if (xQueueReceive(reception_queue, &packet, 10))
        {
            mac80211_frame *p = (mac80211_frame *)packet->payload;
            if (packet->payload[0] < 16)
            {
                uint8_t valor = packet->payload[0];
                p->frame_control.sub_type = valor;
                p->frame_control.type = 0;
                p->frame_control.protocol_version = 0;
            }
            // ESP_LOG_BUFFER_HEXDUMP("netif-rx 802.11  ", packet->payload, packet->rx_ctrl.sig_len - 4, ESP_LOG_INFO);

            // ESP_LOGW(TAG, "TIPO : %x", p->frame_control.type);
            //  ESP_LOGW(TAG, "SUBTIPO : %x", p->frame_control.sub_type);
            memcpy(ap_address, p->transmitter_address, 6);

            connection = search_conection(list_connections, p->transmitter_address);

            if (connection != NULL)
            {
                if (p->frame_control.type == IEEE80211_TYPE_MGT)
                {
                    switch (connection->status)
                    {
                    case PROBE_RESPONSE: // WAIT PACK AUTH REQUEST
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION)
                        {
                            ESP_LOGW(TAG, "Authentication received from=" MACSTR " to= " MACSTR "\n\n", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection, AUTHENTICATION_RESPONSE);
                        }
                        break;
                    case AUTHENTICATION_REQUEST: // WAIT PACK AUTH RESPONSE
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION)
                        {
                            ESP_LOGW(TAG, "Authentication response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection, ASSOCIATION_REQUEST);
                            last_transmission_us = 0;
                        }
                        // solução paleativa
                        else if (p->frame_control.sub_type == AUTHENTICATION_REQUEST)
                        {
                            connection = remove_conection(&list_connections, &connection);
                        }
                        break;
                    case AUTHENTICATION_RESPONSE: // authenticated, wait for association response packet
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ)
                        {
                            ESP_LOGW(TAG, "Association request received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection, ASSOCIATION_RESPONSE);
                        }
                        break;
                    case ASSOCIATION_REQUEST: // associated
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP)
                        {
                            ESP_LOGW(TAG, "Association response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            ESP_LOGW("assoc-data", "Received data frame, will handle");
                            update_conection(connection, CONNECTED);
                        }
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    switch (connection->status)
                    {
                    case ASSOCIATION_RESPONSE: //
                        if (p->frame_control.sub_type == IEEE80211_TYPE_CTL_SUBTYPE_ACK)
                        {
                            ESP_LOGW(TAG, "RECEBI UM ACK from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            ESP_LOGW("assoc-data", "Received data frame, will handle");
                            update_conection(connection, CONNECTED);
                            // last_transmission_us = 0;
                        }
                        break;
                    case CONNECTED:
                        ESP_LOGW(TAG, "Conectado com : " MACSTR "\n", MAC2STR(p->transmitter_address));
                        update_conection(connection, CONNECTED);
                    default:
                        break;
                    }
                }

                free(packet);
            }
        }

        // don't transmit too fast
        if (esp_timer_get_time() - last_transmission_us < time_transmit)
            continue;

        // don't transmit if we don't know how to
        if (!tx)
        {
            ESP_LOGW(TAG, "no transmit function yet");
            continue;
        };

        temp_probe_request++;
        if (temp_probe_request > pre_escale_probe_request)
        {
            // enviando probe request
            ESP_LOGI(TAG, "Sending PROBEREQUEST");
            set_addresses(mac_frame_header_template, IEEE80211_PROBE_REQUEST, ap_address);
            tx(mac_frame_header_template, sizeof(mac_frame_header_template));
            temp_probe_request = 0;
            continue;
        }
        connection = list_connections;

        while (connection != NULL)
        {
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
                ESP_LOGI(TAG, "Sending ACK");
                set_addresses(mac_frame_header_template, IEEE80211_ACK, connection->mac_adress);
                tx(mac_frame_header_template, sizeof(mac_frame_header_template));
                break;
            default:
                break;
            }

            if (esp_timer_get_time() - connection->last_communication_time > time_out)
            {
                connection = remove_conection(&list_connections, &connection);
            }

            if (connection != NULL)
            {
                connection = connection->next;
            }
        }
        last_transmission_us = esp_timer_get_time();
    }
}
