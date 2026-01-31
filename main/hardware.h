#pragma once
#include "esp_wifi.h"

typedef void rx_callback(wifi_promiscuous_pkt_t* packet);

typedef bool tx_func(uint8_t* packet, uint32_t len);

typedef void tx_func_callback(tx_func* t);

typedef struct hardware_mac_args {
	rx_callback* _rx_callback;
    tx_func_callback* _tx_func_callback;
} hardware_mac_args;

void wifi_hardware_task(hardware_mac_args* pvParameter);

typedef void (*tx_done_cb_t)(uint16_t seq, bool success);
void hardware_register_tx_callback(tx_done_cb_t cb);


extern uint8_t module_mac_addr[6];
extern const uint8_t bssid_mac_addr[6];
extern const uint8_t BROADCAST_MAC[6];
#define _MMIO_DWORD(mem_addr) (*(volatile uint32_t *)(mem_addr))
#define _MMIO_ADDR(mem_addr) ((volatile uint32_t*)(mem_addr))
#define MAX_SIZE_PAYLOAD 1500