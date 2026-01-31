#pragma once
#include <stdint.h>
/**
 * QEMU WLAN device emulation
 *
 * Copyright (c) 2008 Clemens Kolbitsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modifications:
 *  2008-February-24  Clemens Kolbitsch :
 *                                  New implementation based on ne2000.c
 *  18/1/22 Martin Johnson : Modified for esp32 wifi emulation
 *  15/10/2023 redfast00: modified for ESP32 wifi implementation
 */

/*         -------------------       Frame Control format ----------------
protocol version   -    Type           -      Subtype    -    To DS  - From DS - More Fragments  - More Fragments - Retry - Power Management
  2 bits                  2bits         -      4bits     -    1bit  -  1bit   -     1bit        -     1 bit      -  1bit -  1bit

|              first byte                                 |       second byte                                                                |


*/
#define QEMU_PACKED __attribute__((packed))

#define IEEE80211_TYPE_MGT 0x00
#define IEEE80211_TYPE_CTL 0x01
#define IEEE80211_TYPE_DATA 0x02

#define IEEE80211_TYPE_MGT_SUBTYPE_BEACON 0x08
#define IEEE80211_TYPE_MGT_SUBTYPE_ACTION 0x0d
#define IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ 0x04
#define IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP 0x05
#define IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION 0x0b
#define IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION 0x0c
#define IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ 0x00
#define IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP 0x01
#define IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION 0x0a

#define IEEE80211_TYPE_DATA_SUBTYPE_DATA 0x00
#define IEEE80211_TYPE_DATA_SUBTYPE_NO_DATA 0x04
#define IEEE80211_TYPE_DATA_SUBTYPE_DATA_QOS 0x08
#define IEEE80211_TYPE_DATA_SUBTYPE_QOS 0x0c

#define IEEE80211_TYPE_CTL_SUBTYPE_ACK 0x0d

#define IEEE80211_PROBE_REQUEST 0x84
#define IEEE80211_PROBE_RESPONSE 0x85
#define IEEE80211_AUTHENTICATION 0x8b
#define IEEE80211_ASSOCIATION_REQ 0x80
#define IEEE80211_ASSOCIATION_RESP 0x81
#define IEEE80211_ACK 0x9d
#define IEEE80211_SEND_DATA 0xA0

#define IEEE80211_TRANSMITTER_ADDR 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
#define IEEE80211_RECIVER_ADDR 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
#define IEEE80211_BSSID 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define IEEE80211_FRAME_CONTROL 0x00, 0x00
#define IEEE80211_DURATION_ID 0x00, 0x00
#define IEEE80211_SEQUENCE_CONTROL 0x00, 0x00
#define IEEE80211_QOS_CONTROL 0x00, 0x00

typedef uint8_t mac_addr_t[6];

#pragma pack(push, 1)


// 1. Controle do Quadro (2 Bytes divididos em bits)
typedef struct
{
    uint16_t protocol_version : 2; // Sempre 0
    uint16_t type : 2;             // Management = 00
    uint16_t subtype : 4;          // Action = 1101 (13 decimal)
    uint16_t to_ds : 1;            // 0 para Ad-Hoc
    uint16_t from_ds : 1;          // 0 para Ad-Hoc
    uint16_t more_frag : 1;        // 0 (geralmente)
    uint16_t retry : 1;            // 1 se for retransmissão
    uint16_t pwr_mgt : 1;          // 0 (Ativo) ou 1 (Power Save)
    uint16_t more_data : 1;        // Usado em buffer AP
    uint16_t protected_frame : 1;  // 0 (Sem criptografia WEP/WPA no cabeçalho)
    uint16_t order : 1;            // 0 (Strictly Ordered)
} mac_frame_control_t;

// 2. Controle de Sequência (2 Bytes divididos em bits)
typedef struct
{
    uint16_t fragment_number : 4;  // Numero do fragmento (0 a 15)
    uint16_t sequence_number : 12; // Numero da sequencia (0 a 4095)
} mac_sequence_control_t;

// 3. Cabeçalho Wi-Fi Padrão (24 Bytes)
typedef struct
{
    mac_frame_control_t frame_control;
    uint16_t duration_id;
    mac_addr_t receiver;    // Destino (Receiver Address)
    mac_addr_t transmitter; // Origem (Transmitter Address)
    mac_addr_t bssid;       // BSSID (No seu caso, Broadcast ou ID da Rede)
    mac_sequence_control_t sequence_control;
} wifi_mac_header_t;

typedef struct
{
    wifi_mac_header_t mac_header;
    uint8_t payload[];
} standard_80211_frame_t;


// 5. Estrutura Completa para Envio (Facilita o Cast e Malloc)
typedef struct
{
    wifi_mac_header_t mac_header;
    uint8_t category; // Fixar em 127 (Vendor Specific)
    uint8_t oui[3];   // Seu OUI: {0x02, 0xAA, 0xBB}
    uint8_t oui_type; // Subtipo do seu protocolo (ex: 0x01)
    
    uint8_t payload[];
} vendor_action_frame_t;

typedef union {
    uint16_t               raw_frame_control; // Acesso rápido aos primeiros 2 bytes
    standard_80211_frame_t std;               // Máscara Padrão
    vendor_action_frame_t  vendor;            // Máscara Vendor
} mac80211_frame;

#pragma pack(pop)

// typedef struct
// {
//     mac_frame_control_t frame_control;
//     uint16_t duration_id;
//     mac_addr_t receiver_address;    // Destino (Receiver Address)
//     mac_addr_t transmitter_address; // Origem (Transmitter Address)
//     mac_addr_t BSSID;               // BSSID (No seu caso, Broadcast ou ID da Rede)
//     mac_sequence_control_t sequence_control;
// } wifi_mac_header_t;