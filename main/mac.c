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



static char* TAG = "mac.c";
static tx_func* tx = NULL;
static QueueHandle_t reception_queue = NULL;

typedef struct openmac_netif_driver* openmac_netif_driver_t;

typedef struct openmac_netif_driver {
    esp_netif_driver_base_t base;
}* openmac_netif_driver_t;

static bool receive_task_is_running = true;
static esp_netif_t *netif_openmac = NULL;

static const char* ssid = "meshtest";
uint8_t ap_address[6];

typedef struct current_connections{
    uint8_t mac_adress[6];   
    openmac_sta_state_t status; 
    uint32_t last_communication_time;
    struct current_connections* next;
} current_connections;

current_connections* list_connections; 


uint8_t to_ap_auth_frame[] = {
    0xb0, 0x00, 
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00, // sequence control
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 0 /*FCS*/};

uint8_t to_ap_assoc_frame_template[] = {
    0x10, 0x00, 
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00, // sequence control
    0x11, 0x00, 0x0a, 0x00, // Fixed parameters
    // SSID
    // supported rates
    // 4 bytes FCS
    };

uint8_t supported_rates[] = {0x01, 0x08, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};  // Supported rates}

uint8_t to_ap_assoc_frame[sizeof(to_ap_assoc_frame_template) + 34 /*2 bytes + 32 byte SSID*/ + sizeof(supported_rates) + 4] = {0};
size_t to_ap_assoc_frame_size = 0;

uint8_t data_frame_template[] = {
    0x08, 0x00, // frame control
    0x00, 0x00, // duration/ID
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // bssid
    0x00, 0x00, // sequence control
    0xaa, 0xaa, // SNAP
    0x03, 0x00, 0x00, 0x00, // other LLC headers
    0xAA, 0xBB // type (AA BB because this needs to be overwritten)
};

uint8_t data_frame_probe_request[] = {
    0x40, 0x00, 
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // receiver addr
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // transmitter addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // bssid
    0x00, 0x00, // sequence control
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 0 /*FCS*/
};


current_connections* create_list_current_connections(){
    return NULL;
}
current_connections* insert_current_connections(current_connections *head, uint8_t mac[6], uint8_t state){
    current_connections *new_conection = malloc(sizeof(current_connections));
    memcpy(new_conection->mac_adress, mac,6);
    new_conection->status =state;
    new_conection->next = NULL;
    new_conection->last_communication_time = esp_timer_get_time()/time_unit;
    
    if(head == NULL){
        head = new_conection;
        return head;
    }
    current_connections *aux =head;
    while(aux->next != NULL){
        aux = aux->next;
    }
    aux->next = new_conection;

    return head;
}
void update_conection(current_connections *conection, uint8_t state){
    conection->status =state;
    conection->last_communication_time = esp_timer_get_time()/time_unit;
}
current_connections* search_conection(current_connections *head,uint8_t mac[6]){
    current_connections* aux= head;
    if(aux != NULL){
        while( memcmp(aux->mac_adress, mac, 6) && aux != NULL){
            aux = aux->next;
        }
        if(aux != NULL){
            return aux;
        }
    }
    return NULL;
}
int search_mac(current_connections *head,uint8_t mac[6]){
    current_connections* aux= head;
    if(aux != NULL){
        while( memcmp(aux->mac_adress, mac, 6) && aux != NULL){
            aux = aux->next;
        }
        if(aux != NULL){
            return true;
        }
    }
    return false;
}

void set_addresses(uint8_t* frame, const uint8_t* ra, const uint8_t* ta, const uint8_t* bssid) {
    // set receiver address
    memcpy(&frame[4], ra, 6);
    // set transmitter address
    memcpy(&frame[10], ta, 6);
    // set bssid
    memcpy(&frame[16], bssid, 6);
}
void set_address_receiver(uint8_t* frame, const uint8_t* ra) {
    // set receiver address
    memcpy(&frame[4], ra, 6);
    
}
void set_address_transmitter(uint8_t* frame, const uint8_t* ta) {
    // set transmitter address
    memcpy(&frame[10], ta, 6);
    
}

void build_frames() {

    set_addresses(to_ap_auth_frame, ap_address, module_mac_addr, ap_address);
    set_address_transmitter(data_frame_probe_request, module_mac_addr);

    memcpy(to_ap_assoc_frame, to_ap_assoc_frame_template, sizeof(to_ap_assoc_frame_template));
    // set SSID
    size_t idx = sizeof(to_ap_assoc_frame_template);

    to_ap_assoc_frame[idx++] = 0x00; // SSID
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) {
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

void open_mac_rx_callback(wifi_promiscuous_pkt_t* packet) {
    mac80211_frame* p = (mac80211_frame*) packet->payload;
    uint8_t mac_listen_probe_request[6] = MAC_PROBE_REQUEST;
    ESP_LOGI(TAG,"Tipo   = %x \n",p->frame_control.type); 
    ESP_LOGI(TAG,"Subtipo= %x \n",p->frame_control.sub_type); 
    // check that receiver mac address matches our mac address or is broadcast
    if ((memcmp(module_mac_addr, p->receiver_address, 6)) && (memcmp(mac_listen_probe_request, p->receiver_address, 6))) { //&& (memcmp(BROADCAST_MAC, p->receiver_address, 6))
        // We're not interested in this packet, return early to avoid having to copy it further to the networking stack
        ESP_LOGD(TAG, "Discarding packet from "MACSTR" to "MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
        return;
   }
   //verificação de probe request
    current_connections *connection_aux = search_conection(list_connections,p->transmitter_address);
    
    if(connection_aux == NULL){ // se o mac ainda nao esta na lsita de conexões
        if(p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ){
            list_connections = insert_current_connections(list_connections,p->transmitter_address, PROBE_RESPONSE);
        }
        else if(p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP){
            list_connections = insert_current_connections(list_connections,p->transmitter_address, AUTHENTICATION_REQUEST);
        }
    }
    else{// se o mac esta na lsita de conexões
        if(p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP && connection_aux->status == PROBE_RESPONSE){
            update_conection(connection_aux,AUTHENTICATION_REQUEST);
            ESP_LOGI(TAG, "Passando para etapa de autenticação");  
         }

    }
    
    ESP_LOGI(TAG, "Accepted: from "MACSTR" to "MACSTR" type=%d, subtype=%d from_ds=%d to_ds=%d", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address), p->frame_control.type, p->frame_control.sub_type, p->frame_control.from_ds, p->frame_control.to_ds);

    if (!reception_queue) {
        ESP_LOGI(TAG, "Received, but queue does not exist yet");
        return;
    }
    // 28 is size of rx_ctrl, 4 is size of FCS (which we don't need)
    wifi_promiscuous_pkt_t* packet_queue_copy = malloc(packet->rx_ctrl.sig_len + 28 - 4);
    memcpy(packet_queue_copy, packet, packet->rx_ctrl.sig_len + 28 - 4);
    if (!(xQueueSendToBack(reception_queue, &packet_queue_copy, 0))) {
        ESP_LOGW(TAG, "MAC RX queue full!");
    }
}

// This function will get called exactly once, with as argument a function (`bool tx_func(uint8_t* packet, uint32_t len)`).
// The function that is passed will TX packets. If it returned `true`, that means that the packet was sent. If false,
//  you'll need to call the function again.
void open_mac_tx_func_callback(tx_func* t) {
    tx = t;
}

void mac_task(void* pvParameters) {
    uint8_t temp_probe_request=0;
    uint64_t last_transmission_us = esp_timer_get_time();
    list_connections = create_list_current_connections();
    current_connections *connection = NULL;

    ESP_LOGI(TAG, "Starting mac_task, running on %d", xPortGetCoreID());
    build_frames();
    ESP_LOGI(TAG, "Built frames, with SSID %s and AP address: %#02x:%#02x:%#02x:%#02x:%#02x:%#02x", ssid, ap_address[0], ap_address[1], ap_address[2], ap_address[3], ap_address[4], ap_address[5]);
    
    reception_queue = xQueueCreate(10, sizeof(wifi_promiscuous_pkt_t*));
    assert(reception_queue);

    while (true) {
        wifi_promiscuous_pkt_t* packet;
        if(xQueueReceive(reception_queue, &packet, 10)) {          
            mac80211_frame* p = (mac80211_frame*) packet->payload;

           
            ESP_LOG_BUFFER_HEXDUMP("netif-rx 802.11", packet->payload, packet->rx_ctrl.sig_len - 4, ESP_LOG_INFO); 
            memcpy(ap_address, p->transmitter_address,6); 

           connection= search_conection(list_connections, p->transmitter_address);
           
           if(connection != NULL){
            ESP_LOGI(TAG, "Entrei no switch"); 
                switch (connection->status){
                 
                    case PROBE_RESPONSE: // WAIT PACK AUTH REQUEST
                        ESP_LOGI(TAG," \n   ------------------         -------------------------\n");
                        if (p->frame_control.type == IEEE80211_TYPE_MGT && p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION) {
                            ESP_LOGW(TAG, "Authentication received from="MACSTR" to= "MACSTR"\n\n", MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection,AUTHENTICATION_RESPONSE);
                            last_transmission_us = 0;
                        }
                        break;
                    case AUTHENTICATION_REQUEST: // WAIT PACK AUTH RESPONSE
                        if (p->frame_control.type == IEEE80211_TYPE_MGT && p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION) {
                            ESP_LOGW(TAG, "Authentication response received from="MACSTR" to= "MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection,ASSOCIATION_REQUEST);
                            last_transmission_us = 0;
                        }
                        break;
                    case AUTHENTICATION_RESPONSE: // authenticated, wait for association response packet
                        if (p->frame_control.type == IEEE80211_TYPE_MGT && p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ) {
                            ESP_LOGW(TAG, "Association request received from="MACSTR" to= "MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            update_conection(connection,ASSOCIATION_RESPONSE);
                            last_transmission_us = 0;
                        }
                        break;
                    case ASSOCIATION_REQUEST: // associated
                        if (p->frame_control.type == IEEE80211_TYPE_MGT && p->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP) {
                            ESP_LOGW(TAG, "Association response received from="MACSTR" to= "MACSTR, MAC2STR(p->transmitter_address), MAC2STR(p->receiver_address));
                            ESP_LOGW("assoc-data", "Received data frame, will handle");
                        }
                        break;
                    case ASSOCIATION_RESPONSE: // associated
                        ESP_LOGI(TAG, "Conectado1");
                        break;
                    case CONNECTED:
                        ESP_LOGI(TAG, "Conectado2");

                    default:
                        break;
                }
            }
            free(packet);
        }
        
        // don't transmit too fast
        if (esp_timer_get_time() - last_transmission_us < time_transmit) continue;

        // don't transmit if we don't know how to
        if (!tx) {
            ESP_LOGW(TAG, "no transmit function yet");
            continue;
        };

        temp_probe_request++;
        if(temp_probe_request > pre_escale_probe_request){
             //enviando probe request
            ESP_LOGI(TAG, "Sending PROBEREQUEST");
            tx(data_frame_probe_request, sizeof(data_frame_probe_request));
            temp_probe_request=0;
            continue;
        }
        connection = list_connections;
        while(connection != NULL){
            switch(connection->status){
                case PROBE_RESPONSE:
                    ESP_LOGI(TAG, "Sending PROBE RESPONSE");
                    data_frame_probe_request[0]=0x50;
                    set_address_receiver(data_frame_probe_request,connection->mac_adress);
                    tx(data_frame_probe_request, sizeof(data_frame_probe_request));
                    data_frame_probe_request[0]=0x40;
                    break;
                case AUTHENTICATION_REQUEST:
                    ESP_LOGI(TAG, "Sending authentication Request!");
                    set_address_receiver(to_ap_auth_frame,connection->mac_adress);
                    tx(to_ap_auth_frame, sizeof(to_ap_auth_frame));
                    break;
                case AUTHENTICATION_RESPONSE:
                    ESP_LOGI(TAG, "Sending authentication response!");
                    set_address_receiver(to_ap_auth_frame,connection->mac_adress);
                    tx(to_ap_auth_frame, sizeof(to_ap_auth_frame));
                    break;
                case ASSOCIATION_REQUEST:
                    ESP_LOGI(TAG, "Sending association request frame!");
                    to_ap_assoc_frame[0]=0x00;
                    set_address_receiver(to_ap_assoc_frame,connection->mac_adress);
                    tx(to_ap_assoc_frame, sizeof(to_ap_assoc_frame));
                    break;
                case ASSOCIATION_RESPONSE:
                    ESP_LOGI(TAG, "Sending association response frame!");
                    to_ap_assoc_frame[0]=0x10;
                    set_address_receiver(to_ap_assoc_frame,connection->mac_adress);
                    tx(to_ap_assoc_frame, sizeof(to_ap_assoc_frame));
                    to_ap_assoc_frame[0]=0x00;
                    break;
                case CONNECTED:
                 ESP_LOGI(TAG, "Conectado");
                    tx(to_ap_assoc_frame, to_ap_assoc_frame_size);
                break;
            }
            connection = connection->next;
            
        }
        last_transmission_us = esp_timer_get_time();
    }
}

// static esp_err_t openmac_netif_transmit(void *h, void *buffer, size_t len)
// {
//     uint8_t* eth_data = (uint8_t*) buffer;
//     ESP_LOGI("netif-tx", "Going to transmit a packet: to "MACSTR" from "MACSTR" type=%02x%02x", MAC2STR(&eth_data[0]), MAC2STR(&eth_data[6]), eth_data[12], eth_data[13]);
//     ESP_LOG_BUFFER_HEXDUMP("netif-tx", eth_data, len, ESP_LOG_INFO);
//     // We need to transform this Ethernet packet to a packet that can be sent via 802.11 data frame
//     // Luckily for us; that's pretty do-able
//     size_t wifi_packet_size = sizeof(data_frame_template) - (6+6+2/*ethernet header*/) + len + 4 /*FCS*/;
//     uint8_t* wifi_data_frame = malloc(wifi_packet_size);
    
//     // Copy over wifi data frame template
//     memcpy(wifi_data_frame, data_frame_template, sizeof(data_frame_template));
//     // Set destination MAC address
//     memcpy(wifi_data_frame + (4+2*6), eth_data, 6);
//     // Set transmitter MAC address
//     memcpy(wifi_data_frame + (4 + 6), eth_data + 6, 6);
//     // Set type
//     memcpy(wifi_data_frame + (sizeof(data_frame_template) - 2), eth_data + (2*6), 2);
//     // Set data
//     memcpy(wifi_data_frame + sizeof(data_frame_template), eth_data + (2*6+2), len - (2*6+2));
//     // Set FCS to 0
//     memset(wifi_data_frame + wifi_packet_size - 4, 0, 4);

//     if (!tx) {
//         return ESP_FAIL;
//     }
//     // TODO check that we have TX slots before transmitting
//     // ESP_LOGI("netif-tx", "transformed packet");
//     // ESP_LOG_BUFFER_HEXDUMP("netif-tx", wifi_data_frame, wifi_packet_size, ESP_LOG_INFO);

//     //tx(wifi_data_frame, wifi_packet_size);
//     free(wifi_data_frame);

//     return ESP_OK;
// }
// static esp_err_t openmac_netif_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
// {
//     return openmac_netif_transmit(h, buffer, len);
// }



// // Free RX buffer (not used as the buffer is static)
// // TODO ^ is this true?
// static void openmac_free(void *h, void* buffer)
// {
//     ESP_LOGI(TAG, "Free-ing RX'd packet %p", buffer);
//     free(buffer);
// }

// static esp_err_t openmac_driver_start(esp_netif_t * esp_netif, void * args)
// {
//     openmac_netif_driver_t driver = args;
//     driver->base.netif = esp_netif;
//     esp_netif_driver_ifconfig_t driver_ifconfig = {
//             .handle =  driver,
//             .transmit = openmac_netif_transmit,
//             .transmit_wrap = openmac_netif_transmit_wrap,
//             .driver_free_rx_buffer = openmac_free
//     };

//     return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
// }


// openmac_netif_driver_t openmac_create_if_driver()
// {
//     openmac_netif_driver_t driver = calloc(1, sizeof(struct openmac_netif_driver));
//     if (driver == NULL) {
//         ESP_LOGE(TAG, "No memory to create a wifi interface handle");
//         return NULL;
//     }
//     driver->base.post_attach = openmac_driver_start;
    
//     // TODO fix this
//     if (!receive_task_is_running) {
//         receive_task_is_running = true;
//     }
//     return driver;
// }

// esp_err_t openmac_netif_start()
// {
//     esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
//     base_cfg.if_desc = "openmac";
//     // base_cfg.get_ip_event = NULL;
//     // base_cfg.lost_ip_event = NULL;

//     esp_netif_config_t cfg = {
//             .base = &base_cfg,
//             .driver = NULL,
//             .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
//     netif_openmac = esp_netif_new(&cfg);
//     assert(netif_openmac);

//     openmac_netif_driver_t driver = openmac_create_if_driver();
//     if (driver == NULL) {
//         ESP_LOGE(TAG, "Failed to create wifi interface handle");
//         return ESP_FAIL;
//     }
//     esp_netif_attach(netif_openmac, driver);
//     esp_netif_set_hostname(netif_openmac, "esp32-open-mac");
//     esp_netif_set_mac(netif_openmac, module_mac_addr);
//     esp_netif_action_start(netif_openmac, NULL, 0, NULL);
//     return ESP_OK;
// }