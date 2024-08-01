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


static const char *ssid = "meshtest";
uint8_t ap_address[6];

typedef struct current_connections
{
    uint8_t mac_adress[6];
    openmac_sta_state_t status;
    uint64_t last_communication_time;
    struct current_connections *next;
} current_connections;

current_connections *list_connections =NULL;


uint8_t supported_rates[] = {0x01, 0x08, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c}; // Supported rates}

uint8_t to_ap_auth_frame[] = {
    0xb0, 0x00,
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00,                         // sequence control
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 0 /*FCS*/};

uint8_t to_ap_assoc_frame_template[] = {
    IEEE80211_ASSOCIATION_RESP, 0x00,
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00,                         // sequence control
    0x11, 0x00, 0x0a, 0x00,             // Fixed parameters
    // SSID
    // supported rates
    // 4 bytes FCS
};



uint8_t to_ap_assoc_frame[sizeof(to_ap_assoc_frame_template) + 34 /*2 bytes + 32 byte SSID*/ + sizeof(supported_rates) + 4] = {0};
size_t to_ap_assoc_frame_size = 0;

uint8_t data_frame_template[] = {
    0x08, 0x00,                         // frame control
    0x00, 0x00,                         // duration/ID
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00,                         // sequence control
    0xaa, 0xaa,                         // SNAP
    0x03, 0x00, 0x00, 0x00,             // other LLC headers
    0xAA, 0xBB                          // type (AA BB because this needs to be overwritten)
};

uint8_t data_frame_probe_request[] = {
    0x40, 0x00,
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // bssid
    0x00, 0x00,                         // sequence control
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 0 /*FCS*/
};
uint8_t data_frame[] = {
    0x0, 0x00,   //
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00,                         // sequence control
    0x11, 0x00, 0x0a, 0x00,             // Fixed parameters
    // SSID
    // supported rates
    // 4 bytes FCS
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
    while(item != NULL && memcmp(item->mac_adress, mac, 6)){
		item= item->next;	
	}
	return item;

}

current_connections *remove_conection(current_connections **head, current_connections **connection_removed)
{
    current_connections *item = *head;
    current_connections *previous_item = NULL;

    while(item != NULL && item != *connection_removed){
		previous_item =item;
		item= item->next;	
	}

	if(item != NULL){   
		if(previous_item == NULL ){
			*head = item->next;
            previous_item =  item->next;
		}else{ 
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

void set_addresses(uint8_t *frame, const uint8_t *ra, const uint8_t *ta, const uint8_t *bssid)
{
    // set receiver address
    memcpy(&frame[4], ra, 6);
    // set transmitter address
    memcpy(&frame[10], ta, 6);
    // set bssid
    memcpy(&frame[16], bssid, 6);
}
void set_address_receiver(uint8_t *frame, const uint8_t *ra)
{
    // set receiver address
    memcpy(&frame[4], ra, 6);
}
void set_address_transmitter(uint8_t *frame, const uint8_t *ta)
{
    // set transmitter address
    memcpy(&frame[10], ta, 6);
}

void build_frames()
{

    set_addresses(to_ap_auth_frame, ap_address, module_mac_addr, ap_address);
    set_address_transmitter(data_frame_probe_request, module_mac_addr);

    memcpy(to_ap_assoc_frame, to_ap_assoc_frame_template, sizeof(to_ap_assoc_frame_template));
    // set SSID
    size_t idx = sizeof(to_ap_assoc_frame_template);

    to_ap_assoc_frame[idx++] = 0x00; // SSID
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32)
    {
        ESP_LOGE(TAG, "Length of SSID %d>32", (int)ssid_len);
        ssid_len = 32;
    }
    to_ap_assoc_frame[idx++] = ssid_len & 0xff;
    memcpy(&to_ap_assoc_frame[idx], ssid, ssid_len);
    idx += ssid_len;
    memcpy(&to_ap_assoc_frame[idx], supported_rates, sizeof(supported_rates));
    idx += sizeof(supported_rates);
    idx += 4; // FCS, value does not matter
    to_ap_assoc_frame_size = idx;

    set_addresses(to_ap_assoc_frame, ap_address, module_mac_addr, ap_address);
    set_addresses(data_frame_template, ap_address, module_mac_addr, ap_address);
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
    current_connections *connection_aux= NULL;

    //ESP_LOGI(TAG, "Tipo   = %x \n", p->frame_control.type);
    //ESP_LOGI(TAG, "Subtipo= %x \n", p->frame_control.sub_type);
    // check that receiver mac address matches our mac address or is broadcast
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

    //ESP_LOGI(TAG, "Accepted: from " MACSTR " to " MACSTR " type=%d, subtype=%d from_ds=%d to_ds=%d", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address), p->frame_control.type, p->frame_control.sub_type, p->frame_control.from_ds, p->frame_control.to_ds);

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

void mac_task(void *pvParameters)
{
    uint8_t temp_probe_request = 0;
    uint64_t last_transmission_us = esp_timer_get_time();
    list_connections = create_list_current_connections();
    current_connections *connection = NULL;

    ESP_LOGI(TAG, "Starting mac_task, running on %d", xPortGetCoreID());
    build_frames();
    ESP_LOGI(TAG, "Built frames, with SSID %s and AP address: %#02x:%#02x:%#02x:%#02x:%#02x:%#02x", ssid, ap_address[0], ap_address[1], ap_address[2], ap_address[3], ap_address[4], ap_address[5]);

    reception_queue = xQueueCreate(10, sizeof(wifi_promiscuous_pkt_t *));
    assert(reception_queue);

    while (true)
    {
        wifi_promiscuous_pkt_t *packet;
        if (xQueueReceive(reception_queue, &packet, 10))
        {
            mac80211_frame *p = (mac80211_frame *)packet->payload;

            ESP_LOG_BUFFER_HEXDUMP("netif-rx 802.11", packet->payload, packet->rx_ctrl.sig_len - 4, ESP_LOG_INFO);
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
                            //last_transmission_us = 0;
                        }
                        break;
                    case AUTHENTICATION_REQUEST: // WAIT PACK AUTH RESPONSE
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION)
                        {
                            ESP_LOGW(TAG, "Authentication response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection, ASSOCIATION_REQUEST);
                            //last_transmission_us = 0;
                        }
                        break;
                    case AUTHENTICATION_RESPONSE: // authenticated, wait for association response packet
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ)
                        {
                            ESP_LOGW(TAG, "Association request received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection, ASSOCIATION_RESPONSE);
                            //last_transmission_us = 0;
                        }
                        break;
                    case ASSOCIATION_REQUEST: // associated
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP)
                        {
                            ESP_LOGW(TAG, "Association response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            ESP_LOGW("assoc-data", "Received data frame, will handle");
                            update_conection(connection, CONNECTED);
                            //last_transmission_us = 0;
                        }
                        break;
                    case ASSOCIATION_RESPONSE: //
                        if (p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ACTION)
                        {
                            ESP_LOGW(TAG, "Association response received from=" MACSTR " to= " MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            ESP_LOGW("assoc-data", "Received data frame, will handle");
                            update_conection(connection, CONNECTED);
                            //last_transmission_us = 0;
                        }
                        break;
                    case CONNECTED:
                        ESP_LOGI(TAG, "Conectado");
                        update_conection(connection, CONNECTED);

                    default:
                        break;
                    }
                }else if(p->frame_control.type == IEEE80211_TYPE_DATA){
                    update_conection(connection, CONNECTED);
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
            tx(data_frame_probe_request, sizeof(data_frame_probe_request));
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
                data_frame_probe_request[0] = IEEE80211_PROBE_RESPONSE;
                set_address_receiver(data_frame_probe_request, connection->mac_adress);
                tx(data_frame_probe_request, sizeof(data_frame_probe_request));
                data_frame_probe_request[0] = IEEE80211_PROBE_REQUEST;
                break;
            case AUTHENTICATION_REQUEST:
                ESP_LOGI(TAG, "Sending authentication Request!");
                set_address_receiver(to_ap_auth_frame, connection->mac_adress);
                tx(to_ap_auth_frame, sizeof(to_ap_auth_frame));
                break;
            case AUTHENTICATION_RESPONSE:
                ESP_LOGI(TAG, "Sending authentication response!");
                set_address_receiver(to_ap_auth_frame, connection->mac_adress);
                tx(to_ap_auth_frame, sizeof(to_ap_auth_frame));
                break;
            case ASSOCIATION_REQUEST:
                ESP_LOGI(TAG, "Sending association request frame!");
                to_ap_assoc_frame[0] = IEEE80211_ASSOCIATION_REQ;
                set_address_receiver(to_ap_assoc_frame, connection->mac_adress);
                tx(to_ap_assoc_frame, sizeof(to_ap_assoc_frame));
                break;
            case ASSOCIATION_RESPONSE:
                ESP_LOGI(TAG, "Sending association response frame!");
                to_ap_assoc_frame[0] = IEEE80211_ASSOCIATION_RESP;
                set_address_receiver(to_ap_assoc_frame, connection->mac_adress);
                tx(to_ap_assoc_frame, sizeof(to_ap_assoc_frame));
                to_ap_assoc_frame[0] = IEEE80211_ASSOCIATION_REQ;
                break;
            case CONNECTED:
                ESP_LOGI(TAG, "ACK");
                to_ap_assoc_frame[0] = IEEE80211_ACK;
                set_address_receiver(to_ap_assoc_frame, connection->mac_adress);
                tx(to_ap_assoc_frame, sizeof(to_ap_assoc_frame));
                to_ap_assoc_frame[0] = IEEE80211_ASSOCIATION_REQ;
                break;
            default:
                break;
            }

            
            if (esp_timer_get_time() - connection->last_communication_time > time_out){
                connection =remove_conection(&list_connections,&connection);
            }
            if(connection != NULL){
                connection = connection->next;
            }
            
        }
        last_transmission_us = esp_timer_get_time();
    }
}