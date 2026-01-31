#include "freertos/FreeRTOS.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "soc/soc.h"
#include "soc/periph_defs.h"
#include "esp32/rom/ets_sys.h"

#include "nvs_flash.h"
#include "string.h"

#include "proprietary.h" // contains all symbols from the binary blobs we still need
#include "hardware.h"
#include "hwinit.h"

#ifndef MAC_TX_HT_SIG_OS
#define MAC_TX_HT_SIG_OS 1 // Verifique o offset correto no seu driver
#define MAC_TX_HT_UNKNOWN_OS 1
#endif

#define RX_BUFFER_AMOUNT 10
static const char *TAG = "hardware.c";
uint8_t module_mac_addr[6]; // MAC DO MEU ESP
const uint8_t bssid_mac_addr[6] = {0xAC, 0xDC, 0xFF, 0xFF, 0xFF, 0x00};
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
#define WIFI_BSSID_FILTER_ADDR_SLOT_0 _MMIO_ADDR(0x3ff73000)
#define MAC_CTRL_REG _MMIO_DWORD(0x3ff73cb8)
static tx_done_cb_t global_tx_cb = NULL;

// Declaração de funções proprietárias (blobs) que vamos usar
void chip_v7_set_chan_nomac(uint8_t channel, uint8_t unknown);
void disable_wifi_agc();
void enable_wifi_agc();
void tx_pwctrl_background(int a, int b);

void hardware_register_tx_callback(tx_done_cb_t cb)
{
	global_tx_cb = cb;
}

inline void write_register(uint32_t address, uint32_t value)
{
	*((volatile uint32_t *)address) = value;
}

inline uint32_t read_register(uint32_t address)
{
	return *((volatile uint32_t *)address);
}

// there are 5 TX slots
// format: _BASE addresses are the base addresses
//         _OS amounts is the amount of 4-byte words in the offset between slots
// So for example, if the MAC_TX_PLCP0 for slot 0 is at 0x3ff73d20
// then the MAC_TX_PLCP0 for slot 1 will be at 0x3ff73d20 - 2 * 4 = 0x3ff73d18

#define MAC_TX_PLCP0_BASE _MMIO_ADDR(0x3ff73d20)
#define MAC_TX_PLCP0_OS (-2)

#define WIFI_TX_CONFIG_BASE _MMIO_ADDR(0x3ff73d1c)
#define WIFI_TX_CONFIG_OS (-2)

#define MAC_TX_PLCP1_BASE _MMIO_ADDR(0x3ff74258)
#define MAC_TX_PLCP1_OS (-0xf)

#define MAC_TX_PLCP2_BASE _MMIO_ADDR(0x3ff7425c)
#define MAC_TX_PLCP2_OS (-0xf)

#define MAC_TX_DURATION_BASE _MMIO_ADDR(0x3ff74268)
#define MAC_TX_DURATION_OS (-0xf)

#define MAC_TX_HT_SIG_BASE _MMIO_ADDR(0x3ff74260)
#define MAC_TX_HT_SIG_OS (-0xf)

#define MAC_TX_HT_UNKNOWN_BASE _MMIO_ADDR(0x3ff74264)
#define MAC_TX_HT_UNKNOWN_OS (-0xf)

#define WIFI_DMA_INT_STATUS _MMIO_DWORD(0x3ff73c48)
#define WIFI_DMA_INT_CLR _MMIO_DWORD(0x3ff73c4c)

#define WIFI_MAC_BITMASK_084 _MMIO_DWORD(0x3ff73084)
#define WIFI_NEXT_RX_DSCR _MMIO_DWORD(0x3ff7308c)
#define WIFI_LAST_RX_DSCR _MMIO_DWORD(0x3ff73090)
#define WIFI_BASE_RX_DSCR _MMIO_DWORD(0x3ff73088)

#define WIFI_TXQ_GET_STATE_COMPLETE _MMIO_DWORD(0x3ff73cc8)
#define WIFI_TXQ_CLR_STATE_COMPLETE _MMIO_DWORD(0x3ff73cc4)

// Collision or timeout
#define WIFI_TXQ_GET_STATE_ERROR _MMIO_DWORD(0x3ff73cc0)
#define WIFI_TXQ_CLR_STATE_ERROR _MMIO_DWORD(0x3ff73cbc)

#define WIFI_MAC_ADDR_SLOT_0 0x3ff73040
#define WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 0x3ff73064

#define WIFI_BSSID_FILTER_ADDR_SLOT_0 _MMIO_ADDR(0x3ff73000)

#define MAC_CTRL_REG _MMIO_DWORD(0x3ff73cb8)

typedef struct __attribute__((packed)) dma_list_item
{
	uint16_t size : 12;
	uint16_t length : 12;
	uint8_t _unknown : 6;
	uint8_t has_data : 1;
	uint8_t owner : 1;
	void *packet;
	struct dma_list_item *next;
} dma_list_item;

typedef enum
{
	RX_ENTRY,
	TX_ENTRY,
	CHANGE_CHANNEL_ENTRY
} hardware_queue_entry_type_t;

typedef struct
{
	uint32_t interrupt_received;
} rx_queue_entry_t;

typedef struct
{
	uint8_t *packet;
	uint32_t len;
} tx_queue_entry_t;

typedef struct
{
	uint8_t channel;
} change_channel_queue_entry_t;

typedef struct
{
	hardware_queue_entry_type_t type;
	union
	{
		rx_queue_entry_t rx;
		tx_queue_entry_t tx;
		change_channel_queue_entry_t change_channel;
	} content;
} hardware_queue_entry_t;

SemaphoreHandle_t tx_queue_resources = NULL;
SemaphoreHandle_t rx_queue_resources = NULL;

QueueHandle_t hardware_event_queue = NULL;

dma_list_item *rx_chain_begin = NULL;
dma_list_item *rx_chain_last = NULL;

volatile int interrupt_count = 0;

#define TX_SLOT_CNT 5

static uint16_t current_hw_seq = 0;

typedef struct
{
	// dma_list_item must be 4-byte aligned (it's passed to hardware that only takes those addresses)
	struct
	{
	} __attribute__((aligned(4)));
	dma_list_item dma;

	tx_queue_entry_t packet;
	bool in_use;
} tx_hardware_slot_t;

tx_hardware_slot_t tx_slots[TX_SLOT_CNT] = {};

// --- INICIO DO BLOCO NOVO ---

// Funções para reiniciar o MAC (necessário para troca de canal)
static void init_mac()
{
	MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800;
}

static void deinit_mac()
{
	MAC_CTRL_REG = MAC_CTRL_REG | 0x17ff;
	while ((MAC_CTRL_REG & 0x2000) != 0)
	{
		// espera desativar
	}
}

// Função pública para solicitar troca de canal
void request_channel_change(uint8_t channel)
{
	hardware_queue_entry_t msg;
	msg.type = CHANGE_CHANNEL_ENTRY;
	msg.content.change_channel.channel = channel;

	if (xQueueSendToBack(hardware_event_queue, &msg, 0) != pdTRUE)
	{
		ESP_LOGE(TAG, "Falha ao enfileirar troca de canal");
	}
}

// A lógica física de troca de canal
static void change_channel_to(uint8_t channel)
{
	ESP_LOGI(TAG, "Trocando para canal %d", channel);
	if (channel < 1 || channel > 14)
	{
		ESP_LOGE(TAG, "Canal invalido");
		return;
	}
	deinit_mac();
	chip_v7_set_chan_nomac(channel, 0);
	disable_wifi_agc();
	init_mac();
	enable_wifi_agc();
}

// Funções de Filtro BSSID (Correção do Broadcast)
static void set_enable_bssid_filter(uint8_t slot, bool enable)
{
	assert(slot <= 1);
	if (enable)
	{
		*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 9) |= 0x10000;
	}
	else
	{
		*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 9) &= ~(0x10000);
	}
}

static void set_bssid_filter(uint8_t slot, const uint8_t *addr)
{
	assert(slot <= 1);
	// desabilita para atualizar seguro
	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 9) &= 0xfffeffff;

	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2) = addr[0] | addr[1] << 8 | addr[2] << 16 | addr[3] << 24;
	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 1) = addr[4] | addr[5] << 8;
	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 8) = ~0;
	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 9) = 0xffff;

	// habilita
	*(WIFI_BSSID_FILTER_ADDR_SLOT_0 + slot * 2 + 9) |= 0x10000;
}

// Habilita recepção de frames que normalmente são filtrados
void set_some_kind_of_rx_policy(uint8_t slot, bool enable)
{
	assert(slot <= 1);
	if (enable)
	{
		*(volatile uint32_t *)(0x3ff730d8 + 4 * slot) |= 0x110;
	}
	else
	{
		*(volatile uint32_t *)(0x3ff730d8 + 4 * slot) &= ~0x110;
	}
}
// --- FIM DO BLOCO NOVO ---

void log_dma_item(dma_list_item *item)
{
	ESP_LOGD("dma_item", "cur=%p owner=%d has_data=%d length=%d size=%d packet=%p next=%p", item, item->owner, item->has_data, item->length, item->size, item->packet, item->next);
}

dma_list_item tx_item_u;
dma_list_item *tx_item = &tx_item_u;

// bool transmit_packet(uint8_t *tx_buffer, uint32_t buffer_len)
// {
// 	uint32_t slot = 0;

// 	// Find the first free TX slot
// 	for (slot = 0; slot < TX_SLOT_CNT; slot++)
// 	{
// 		if (!tx_slots[slot].in_use)
// 		{
// 			break;
// 		}
// 	}
// 	if (slot == TX_SLOT_CNT)
// 	{
// 		ESP_LOGE(TAG, "all tx slots full");
// 		return false;
// 	}
// 	ESP_LOGI(TAG, "using tx slot %d", (int)slot);

// 	dma_list_item *tx_item = &(tx_slots[slot].dma);
// 	// dma_list_item must be 4-byte aligned (it's passed to hardware that only takes those addresses)
// 	assert(((uint32_t)(tx_item) & 0b11) == 0);
// 	tx_buffer[2] = 0x30;
// 	tx_buffer[3] = 0x00;
// 	tx_slots[slot].in_use = true;
// 	tx_slots[slot].packet.packet = tx_buffer;
// 	tx_slots[slot].packet.len = buffer_len;

// 	uint32_t size_len = buffer_len + 32;

// 	ESP_LOGI(TAG, "len=%d", (int)buffer_len);
// 	ESP_LOG_BUFFER_HEXDUMP("to-transmit", tx_slots[slot].packet.packet, buffer_len, ESP_LOG_INFO);

// 	tx_item->owner = 1;
// 	tx_item->has_data = 1;
// 	tx_item->length = buffer_len;
// 	tx_item->size = size_len;
// 	tx_item->packet = tx_buffer;
// 	tx_item->next = NULL;

// 	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] = WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] | 0xa;

// 	MAC_TX_PLCP0_BASE[MAC_TX_PLCP0_OS * slot] = (((uint32_t)(tx_item)) & 0xfffff) | (0x00600000);
// 	// uint32_t rate = WIFI_PHY_RATE_54M; // see wifi_phy_rate_t
// 	uint32_t rate = WIFI_PHY_RATE_1M_L;
// 	uint32_t is_n_enabled = (rate >= 16);

// 	MAC_TX_PLCP1_BASE[MAC_TX_PLCP1_OS * slot] = 0x10000000 | (buffer_len & 0xfff) | ((rate & 0x1f) << 12) | ((is_n_enabled & 0b1) << 25);
// 	MAC_TX_PLCP2_BASE[MAC_TX_PLCP2_OS * slot] = 0x00000020;
// 	MAC_TX_DURATION_BASE[MAC_TX_DURATION_OS * slot] = 0x30;

// 	if (is_n_enabled)
// 	{
// 		ESP_LOGE(TAG, "we don't support 802.11n yet; see mac_tx_set_htsig in binary blob"); //  TODO
// 		abort();
// 	}

// 	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] |= 0x02000000;
// 	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] |= 0x00003000;

// 	// Transmit: setting the 0xc0000000 bit in MAC_TX_PLCP0 enables transmission
// 	MAC_TX_PLCP0_BASE[MAC_TX_PLCP0_OS * slot] |= 0xc0000000;
// 	return true;
// }
bool transmit_packet_advanced(uint8_t *tx_buffer, uint32_t buffer_len, uint32_t rate_enum)
{
	uint32_t slot = 0;
	// 1. Busca Slot Livre
	for (slot = 0; slot < TX_SLOT_CNT; slot++)
	{
		if (!tx_slots[slot].in_use)
			break;
	}
	if (slot == TX_SLOT_CNT)
	{
		ESP_LOGE(TAG, "all tx slots full");
		return false;
	}

	dma_list_item *tx_item = &(tx_slots[slot].dma);
	// dma_list_item must be 4-byte aligned (it's passed to hardware that only takes those addresses)
	assert(((uint32_t)(tx_item) & 0b11) == 0);

	// 3. Configura o Slot
	tx_slots[slot].in_use = true;
	tx_slots[slot].packet.packet = tx_buffer;
	tx_slots[slot].packet.len = buffer_len;

	tx_item->owner = 1;
	tx_item->has_data = 1;
	tx_item->length = buffer_len;
	tx_item->size = buffer_len + 32;
	tx_item->packet = tx_buffer;
	tx_item->next = NULL;

	// 4. Configurações Base
	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] = WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] | 0xa;
	MAC_TX_PLCP0_BASE[MAC_TX_PLCP0_OS * slot] = (((uint32_t)(tx_item)) & 0xfffff) | (0x00600000);

	// 5. Lógica de Velocidade (A parte nova que você trouxe)
	// Ex: WIFI_PHY_RATE_1M_L (0x00) ou WIFI_PHY_RATE_MCS0 (0x10)
	uint32_t rate = rate_enum;
	uint32_t is_ht = (rate >= 0x10);	   // É 802.11n?
	uint32_t is_short_gi = (rate >= 0x18); // Short Guard Interval?
	uint32_t crypto_key_slot = 0;

	MAC_TX_PLCP1_BASE[MAC_TX_PLCP1_OS * slot] = 0x10000000 | (buffer_len & 0xfff) | ((rate & 0x1f) << 12) | ((is_ht & 0b1) << 25) | ((crypto_key_slot & 0b11111) << 17);
	MAC_TX_PLCP2_BASE[MAC_TX_PLCP2_OS * slot] = 0x00000020;
	// MAC_TX_PLCP2_BASE[MAC_TX_PLCP2_OS * slot] = 0x00000004;

	MAC_TX_DURATION_BASE[MAC_TX_DURATION_OS * slot] = 0x00;

	// 7. Configuração Avançada para 802.11n (HT) - "A Novidade"
	if (is_ht)
	{
		// Se for MCS0-7, configura os registradores HT
		uint32_t ht_sig = 0;
		ht_sig |= rate & 0b111;				  // MCS (0-7)
		ht_sig |= 0b0 << 7;					  // 20 MHz (0) ou 40 MHz (1)
		ht_sig |= (buffer_len & 0xffff) << 8; // HT Length
		ht_sig |= 1 << 24;					  // Smoothing recommended
		ht_sig |= 1 << 25;					  // Not sounding
		ht_sig |= 1 << 26;					  // Reserved
		ht_sig |= 0b0 << 27;				  // Aggregation (AMPDU) - Desligado para raw
		ht_sig |= 0b00 << 28;				  // STBC / Spatial stream
		ht_sig |= 0b0 << 30;				  // LDPC (Coding)
		ht_sig |= is_short_gi << 31;		  // Short GI

		// Esses registradores só existem no ESP32-S2/S3/C3 ou nas revs novas
		// Se der erro de compilação, você precisa achar o endereço base deles no seu header
		MAC_TX_HT_SIG_BASE[MAC_TX_HT_SIG_OS * slot] = ht_sig;
		MAC_TX_HT_UNKNOWN_BASE[MAC_TX_HT_UNKNOWN_OS * slot] = (buffer_len & 0xffff) | 0x50000;
	}

	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] |= 0x02000000;
	WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] |= 0x00003000;
	// WIFI_TX_CONFIG_BASE[WIFI_TX_CONFIG_OS * slot] = 0x0200200A;
	//  Transmit: setting the 0xc0000000 bit in MAC_TX_PLCP0 enables transmission
	MAC_TX_PLCP0_BASE[MAC_TX_PLCP0_OS * slot] |= 0xc0000000;

	return true;
}

static void processTxComplete()
{
	// 1. Verificar quem falhou (Timeout ou Colisão)
	// Esse registrador contém os slots que terminaram com ERRO (Sem ACK)
	uint32_t txq_state_error = WIFI_TXQ_GET_STATE_ERROR;

	// 2. Verificar quem teve sucesso (Recebeu ACK)
	uint32_t txq_state_complete = WIFI_TXQ_GET_STATE_COMPLETE;
	ESP_LOGE("TX", " codigin: %lx  | error:%lx ", WIFI_TXQ_GET_STATE_COMPLETE, WIFI_TXQ_GET_STATE_ERROR);
	// --- TRATAMENTO DE ERROS (SEM ACK) ---
	if (txq_state_error != 0)
	{
		uint32_t slot_err = 31 - __builtin_clz(txq_state_error);

		ESP_LOGE("TX_FAIL", "Falha no Slot %lu! (Sem ACK ou Timeout)", slot_err);

		// Limpa o estado de erro do hardware
		uint32_t clear_mask = 1 << slot_err;
		WIFI_TXQ_CLR_STATE_ERROR |= clear_mask;

		// Libera o slot (mesmo falhando, o slot está ocupado e precisa ser solto)
		if (slot_err < TX_SLOT_CNT)
		{
			tx_slots[slot_err].in_use = false;
			// Opcional: Re-enfileirar para tentar de novo (Retry)
			if (tx_slots[slot_err].packet.packet != NULL)
			{
				free(tx_slots[slot_err].packet.packet);
				tx_slots[slot_err].packet.packet = NULL;
			}
		}
	}

	// --- TRATAMENTO DE SUCESSO (COM ACK) ---
	if (txq_state_complete != 0)
	{
		uint32_t slot_ok = 31 - __builtin_clz(txq_state_complete);

		// Se o slot apareceu no COMPLETE e não no ERROR, é vitória real.
		// Às vezes o bit acende nos dois se houver retransmissão, mas ERROR tem prioridade.
		if ((txq_state_error & (1 << slot_ok)) == 0)
		{
			ESP_LOGW("TX_DONE", "SUCESSO! Slot %lu recebeu ACK confirmado!", slot_ok);
		}

		// Limpa o estado de sucesso
		uint32_t clear_mask = 1 << slot_ok;
		WIFI_TXQ_CLR_STATE_COMPLETE |= clear_mask;

		if (slot_ok < TX_SLOT_CNT)
		{
			tx_slots[slot_ok].in_use = false;
			if (tx_slots[slot_ok].packet.packet != NULL)
			{
				free(tx_slots[slot_ok].packet.packet);
				tx_slots[slot_ok].packet.packet = NULL;
			}
		}
	}
	ESP_LOGE("TX", " codigin: %lx  | error:%lx ", WIFI_TXQ_GET_STATE_COMPLETE, WIFI_TXQ_GET_STATE_ERROR);
	// uint32_t txq_state_complete = WIFI_TXQ_GET_STATE_COMPLETE;
	// uint32_t txq_state_error = WIFI_TXQ_GET_STATE_ERROR;
	// ESP_LOGW(TAG, "tx complete = %lx", txq_state_complete);
	// ESP_LOGW(TAG, "ERROR = %lx", txq_state_error);
	// if (txq_state_complete == 0)
	// {
	// 	return;
	// }
	// static uint8_t ctr = 0;
	// ctr++;
	// if (ctr % 4 == 0)
	// {
	// 	tx_pwctrl_background(1, 0);
	// }
	// uint32_t slot = 31 - __builtin_clz(txq_state_complete);
	// // ESP_LOGW(TAG, "slot %lx is now free again", slot);
	// uint32_t clear_mask = 1 << slot;
	// WIFI_TXQ_CLR_STATE_COMPLETE |= clear_mask;
	// if (slot < TX_SLOT_CNT)
	// {
	// 	tx_slots[slot].in_use = false;
	// 	free(tx_slots[slot].packet.packet);
	// 	tx_slots[slot].packet.packet = NULL;
	// }
}

void IRAM_ATTR wifi_interrupt_handler(void *args)
{
	interrupt_count++;
	uint32_t cause = WIFI_DMA_INT_STATUS;
	if (cause == 0)
	{
		return;
	}
	WIFI_DMA_INT_CLR = cause;

	if (cause & 0x800)
	{
		// TODO handle this with open-source code
		// wdev_process_panic_watchdog() is the closed-source way to recover from this
	}
	BaseType_t high_task_awoken = pdFALSE;
	if (xSemaphoreTakeFromISR(rx_queue_resources, &high_task_awoken))
	{
		hardware_queue_entry_t queue_entry;
		queue_entry.type = RX_ENTRY;
		queue_entry.content.rx.interrupt_received = cause;
		xQueueSendFromISR(hardware_event_queue, &queue_entry, &high_task_awoken);
	}
	if (high_task_awoken == pdTRUE)
	{
		portYIELD_FROM_ISR(high_task_awoken);
	}
}

void setup_interrupt()
{
	// See the documentation of intr_matrix_set in esp-idf/components/esp_rom/include/esp32s3/rom/ets_sys.h
	intr_matrix_set(0, ETS_WIFI_MAC_INTR_SOURCE, ETS_WMAC_INUM);

	// Replace the existing wDev_ProcessFiq interrupt
	xt_set_interrupt_handler(ETS_WMAC_INUM, wifi_interrupt_handler, NULL);
	xt_ints_on(1 << ETS_WMAC_INUM);
}

void print_rx_chain(dma_list_item *item)
{
	// Debug print to display RX linked list
	int index = 0;
	ESP_LOGD("rx-chain", "base=%p next=%p last=%p", (dma_list_item *)WIFI_BASE_RX_DSCR, (dma_list_item *)WIFI_NEXT_RX_DSCR, (dma_list_item *)WIFI_LAST_RX_DSCR);
	while (item)
	{
		ESP_LOGD("rx-chain", "idx=%d cur=%p owner=%d has_data=%d length=%d size=%d packet=%p next=%p", index, item, item->owner, item->has_data, item->length, item->size, item->packet, item->next);
		item = item->next;
		index++;
	}
	ESP_LOGD("rx-chain", "base=%p next=%p last=%p", (dma_list_item *)WIFI_BASE_RX_DSCR, (dma_list_item *)WIFI_NEXT_RX_DSCR, (dma_list_item *)WIFI_LAST_RX_DSCR);
}

void set_rx_base_address(dma_list_item *item)
{
	WIFI_BASE_RX_DSCR = (uint32_t)item;
}

void setup_rx_chain()
{
	// This function sets up the linked list needed for the Wi-Fi MAC RX functionality
	dma_list_item *prev = NULL;
	for (int i = 0; i < RX_BUFFER_AMOUNT; i++)
	{
		dma_list_item *item = malloc(sizeof(dma_list_item));
		item->has_data = 0;
		item->owner = 1;
		item->size = MAX_SIZE_PAYLOAD;
		item->length = item->size;

		uint8_t *packet = malloc(MAX_SIZE_PAYLOAD); // TODO verify that this does not need to be bigger
		item->packet = packet;
		item->next = prev;
		prev = item;
		if (!rx_chain_last)
		{
			rx_chain_last = item;
		}
	}
	set_rx_base_address(prev);
	rx_chain_begin = prev;
}

void update_rx_chain()
{
	WIFI_MAC_BITMASK_084 |= 0x1;
	// Wait for confirmation from hardware
	while (WIFI_MAC_BITMASK_084 & 0x1)
		;
}

void handle_rx_messages(rx_callback rxcb)
{
	dma_list_item *current = rx_chain_begin;

	// This is a workaround for when we receive a lot of packets; otherwise we get stuck in this function,
	// handling packets for all eternity
	// This is much less of a problem now that we implement hardware filtering
	int received = 0;
	while (current)
	{
		dma_list_item *next = current->next;
		if (current->has_data)
		{
			// TODO enable interrupt

			received++;
			// Has data, but actual 802.11 MAC frame only starts at 28 bytes into the packet
			// The data before contains packet metadata
			wifi_promiscuous_pkt_t *packet = current->packet;
			// packet->rx_ctrl.sig_len includes the FCS (4 bytes), but we don't need this

			// call callback of upper layer
			rxcb(packet);

			// Recycle DMA item and buffer
			rx_chain_begin = current->next;
			current->next = NULL;
			current->length = current->size;
			current->has_data = 0;

			// This puts the DMA buffer back in the linked list
			// TODO: this code looks pretty ugly and might not be optimal
			if (rx_chain_begin)
			{
				rx_chain_last->next = current;
				update_rx_chain();
				if (WIFI_NEXT_RX_DSCR == 0x3ff00000)
				{
					dma_list_item *last_dscr = (dma_list_item *)WIFI_LAST_RX_DSCR;
					if (current == last_dscr)
					{
						rx_chain_last = current;
					}
					else
					{
						set_rx_base_address(last_dscr->next);
						rx_chain_last = current;
					}
				}
				else
				{
					rx_chain_last = current;
				}
			}
			else
			{
				rx_chain_begin = current;
				set_rx_base_address(current);
				rx_chain_last = current;
			}
			// TODO disable interrupt
		}
		current = next;
		if (received > 10)
		{
			goto out;
		}
	}
out:
	// TODO enable interrupt
}

// Copies packet content to internal buffer, so you can free `packet` immediately after calling this function
static bool wifi_hardware_tx_func(uint8_t *packet, uint32_t len)
{
	if (!xSemaphoreTake(tx_queue_resources, 1))
	{
		ESP_LOGE(TAG, "TX semaphore full!");
		return false;
	}
	uint8_t *queue_copy = (uint8_t *)malloc(len);
	memcpy(queue_copy, packet, len);
	hardware_queue_entry_t queue_entry;
	queue_entry.type = TX_ENTRY;
	queue_entry.content.tx.len = len;
	queue_entry.content.tx.packet = queue_copy;
	xQueueSendToBack(hardware_event_queue, &queue_entry, 0);

	ESP_LOGI("mac-interface", "TX entry queued");
	return true;
}

static void set_enable_mac_addr_filter(uint8_t slot, bool enable)
{
	// This will allow packets that match the filter to be queued in our reception queue
	// will also ack them once they arrive
	assert(slot <= 1);
	uint32_t addr = WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 + 8 * slot;
	if (enable)
	{
		write_register(addr, read_register(addr) | 0x10000);
	}
	else
	{
		write_register(addr, read_register(addr) & ~(0x10000));
	}
}

static void set_mac_addr_filter(uint8_t slot, uint8_t *addr)
{
	assert(slot <= 1);
	write_register(WIFI_MAC_ADDR_SLOT_0 + slot * 8, addr[0] | addr[1] << 8 | addr[2] << 16 | addr[3] << 24);
	write_register(WIFI_MAC_ADDR_SLOT_0 + slot * 8 + 4, addr[4] | addr[5] << 8);
	write_register(WIFI_MAC_ADDR_SLOT_0 + slot * 8 + 8 * 4, ~0);																	// mask bits
	write_register(WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 + slot * 8, read_register(WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 + slot * 8) | 0xffff); // mask bits
}

void wifi_hardware_task(hardware_mac_args *pvParameter)
{
	// // Print MAC addresses
	// for (int i = 0; i < 2; i++)
	// {
	// 	uint8_t mac[6] = {0};
	// 	if (esp_wifi_get_mac(i, mac) == ESP_OK)
	// 	{
	// 		ESP_LOGW(TAG, "MAC %d = %02x:%02x:%02x:%02x:%02x:%02x", i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	// 	}
	// }
	void hal_mac_tsf_reset(uint8_t a);
	hardware_event_queue = xQueueCreate(RX_BUFFER_AMOUNT + 10, sizeof(hardware_queue_entry_t));
	assert(hardware_event_queue);
	rx_queue_resources = xSemaphoreCreateCounting(RX_BUFFER_AMOUNT, RX_BUFFER_AMOUNT);
	assert(rx_queue_resources);
	tx_queue_resources = xSemaphoreCreateCounting(10, 10);
	assert(tx_queue_resources);

	hwinit();

	// From here, we start taking over the hardware; no more proprietary code is executed from now on
	setup_interrupt();

	setup_rx_chain();

	// Slot 1: Configuração para MEUS pacotes (Unicast com ACK)
	set_mac_addr_filter(0, (uint8_t *)BROADCAST_MAC); // Tem que bater com addr2 do pacote
	set_bssid_filter(0, (uint8_t *)bssid_mac_addr);	  // Tem que bater com addr3 do pacote
	set_enable_mac_addr_filter(0, true);
	set_enable_bssid_filter(0, true);
	set_some_kind_of_rx_policy(0, false); // ATIVA ACK
	hal_mac_tsf_reset(1);				  // Sincroniza o relógio
	WIFI_MAC_BITMASK_084 |= 0x80000000;
	// Slot 1: Configuração para MEUS pacotes (Unicast com ACK)
	set_mac_addr_filter(1, module_mac_addr);		// Tem que bater com addr2 do pacote
	set_bssid_filter(1, (uint8_t *)bssid_mac_addr); // Tem que bater com addr3 do pacote
	set_enable_mac_addr_filter(1, true);
	set_enable_bssid_filter(1, true);
	set_some_kind_of_rx_policy(1, true); // ATIVA ACK

	ESP_LOGW(TAG, "Starting to receive messages");

	// ---------------------------------------------------------
	pvParameter->_tx_func_callback(&wifi_hardware_tx_func);

	ESP_LOGW(TAG, "Solicitando troca para Canal 5...");
	request_channel_change(5);
	while (true)
	{
		hardware_queue_entry_t queue_entry;
		if (xQueueReceive(hardware_event_queue, &(queue_entry), 10))
		{

			if (queue_entry.type == RX_ENTRY)
			{

				uint32_t cause = queue_entry.content.rx.interrupt_received;
				// printf("\n Este é o codigo da interrupção: %x  \n" , );
				// ESP_LOGW(TAG, "interrupt = 0x%08lx", cause);
				if (cause & 0x800)
				{
					// Watchdog panic
					// TODO process this
					// TODO what pets this watchdog?
					// ESP_LOGW(TAG, "watchdog panic, how do we pet it?");
				}
				if (cause & 0x600000)
				{
					// TODO this is bad, we should reboot
					ESP_LOGE(TAG, "something bad, we should reboot");
				}
				if (cause & 0x1000024)
				{
					ESP_LOGW(TAG, "HW RX");
					handle_rx_messages(pvParameter->_rx_callback);
				}

				if (cause & 0x80000)
				{
					ESP_LOGE(TAG, "lmacProcessAllTxTimeout");
				}
				if (cause & 0x80)
				{
					ESP_LOGE(TAG, "PROCESS TX COMPLETO");

					processTxComplete();
				}

				if (cause & 0x100)
				{
					ESP_LOGE(TAG, "lmacProcessCollisions");
				}
				xSemaphoreGive(rx_queue_resources);
			}
			else if (queue_entry.type == TX_ENTRY)
			{
				// ESP_LOGI(TAG, "TX from queue");
				//  TODO: implement retry

				uint8_t *raw_pkt = (uint8_t *)queue_entry.content.tx.packet;
				uint16_t seq_ctrl = raw_pkt[22] | (raw_pkt[23] << 8);
				uint8_t *buffer = queue_entry.content.tx.packet;
				uint32_t len = queue_entry.content.tx.len;
				// Descarta os 4 bits de fragmento (shift right 4)
				current_hw_seq = (seq_ctrl >> 4);
				bool sent = transmit_packet_advanced(buffer, len, WIFI_PHY_RATE_MCS0_LGI);

				if (sent)
				{
					// ESP_LOGI(TAG, "Enviado para o Hardware via DMA");
				}
				else
				{
					ESP_LOGE(TAG, "Falha ao enviar (Slots cheios?)");
				}

				// transmit_packet(queue_entry.content.tx.packet, queue_entry.content.tx.len);
				xSemaphoreGive(tx_queue_resources);
			}
			else if (queue_entry.type == CHANGE_CHANNEL_ENTRY)
			{
				change_channel_to(queue_entry.content.change_channel.channel);
			}
			else
			{
				ESP_LOGI(TAG, "unknown queue type");
			}
		}
		// ESP_LOGW(TAG, "interrupt count=%d", interrupt_count);
	}
}
