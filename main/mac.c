#include "esp_wifi.h"
#include "esp_mac.h"
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

#include "freertos/FreeRTOS.h" // Núcleo do FreeRTOS
#include "freertos/task.h"     // Para vTaskDelay, xTaskCreate
#include "freertos/queue.h"    // Para xQueueCreate
#include "freertos/timers.h"   //

#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARG(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MAX_PENDING_PACKETS 5 // Quantos pacotes "voando" ao mesmo tempo
#define MAX_RETRIES 3
#define MAX_NEIGHBORS 10

// --- CONFIGURAÇÕES DE BACKOFF ---
#define SLOT_TIME_US 1000 // 1ms (Tempo base do slot de software)
#define CW_MIN 15         // Janela inicial (0..15 slots)
#define CW_MAX 1023       // Janela máxima (0..1023 slots)

// --- BEACON PERIODICO
uint16_t hello_interval_ms = 5000;
static TimerHandle_t hello_timer = NULL;

// variaveis globais
static const char *TAG = "MAC";
static tx_func *hardware_tx_func = NULL;
mac_addr_t bssid = "OPPNET";
static const char *TAG_DUMP = "MAC_DUMP";
static const uint8_t MY_OUI[] = {0x02, 0xAA, 0xBB};
static neighbor_cache_t dedup_cache[MAX_NEIGHBORS];

static QueueHandle_t rx_queue_hw_to_mac = NULL; // Fila RX (Hardware -> MAC)
QueueHandle_t mac_event_queue = NULL;
// --- FUNÇÃO AUXILIAR: CONSTRÓI E ENVIA FRAME (Genérica) ---
// Retorna true se o hardware aceitou (colocou na fila), false se fila cheia/erro
bool build_and_send_frame(uint8_t *dest_mac, uint8_t *payload, uint16_t len, uint16_t seq_num)
{

    // 1. Verificação de Segurança
    if (!hardware_tx_func)
    {
        ESP_LOGE(TAG, "Hardware TX func is NULL");
        return false;
    }

    // 2. Cálculo do Tamanho Total
    // Header Wi-Fi (24) + Vendor Header (5) + Payload Útil (len)
    // Nota: O FCS (CRC de 4 bytes) é adicionado automaticamente pelo hardware, não alocamos aqui.
    uint16_t total_len = sizeof(wifi_mac_header_t) + 5 + len;

    // 3. Alocação de Memória Temporária
    // Precisamos de um buffer contíguo para passar pro driver
    uint8_t *tx_buf = malloc(total_len);
    if (!tx_buf)
    {
        ESP_LOGE(TAG, "Falha de Malloc no build_frame");
        return false;
    }

    // Zera o buffer para evitar lixo de memória
    memset(tx_buf, 0, total_len);

    // Cast para facilitar o preenchimento usando a struct
    vendor_action_frame_t *frame = (vendor_action_frame_t *)tx_buf;

    // --- 4. PREENCHIMENTO DO CABEÇALHO 802.11 ---

    // Frame Control: Management (00) / Action (1101 = 13)
    frame->mac_header.frame_control.protocol_version = 0;
    frame->mac_header.frame_control.type = 0;    // data
    frame->mac_header.frame_control.subtype = 13; // 
    frame->mac_header.frame_control.to_ds = 0;   // Ad-Hoc
    frame->mac_header.frame_control.from_ds = 0; // Ad-Hoc

    // Endereços
    memcpy(frame->mac_header.receiver, dest_mac, 6);
    memcpy(frame->mac_header.transmitter, module_mac_addr, 6); // Definido no hardware.c
    memcpy(frame->mac_header.bssid, bssid_mac_addr, 6);        // Em Ad-Hoc/Mesh, BSSID geralmente é o destino ou Broadcast

    // Duration (Opcional, geralmente 0 ou tempo para NAV)
    frame->mac_header.duration_id = 0x0000;

    frame->mac_header.sequence_control.fragment_number = 0;
    frame->mac_header.sequence_control.sequence_number = seq_num;

    // --- 5. PREENCHIMENTO DO CABEÇALHO VENDOR ---
    frame->category = 127;              // Vendor Specific Category
    memcpy(frame->oui, MY_OUI, 3);      // Seu OUI (02:AA:BB)
    frame->oui_type = OPPNET_LINK_DATA; // 0x01 (Sinaliza que é DADO, não ACK)

    // --- 6. CÓPIA DO PAYLOAD ---

    // O payload começa logo após o cabeçalho vendor.
    // A struct 'vendor_action_frame_t' tem um membro 'payload[]' flexível no fim.
    memcpy(frame->payload, payload, len);

    // --- 7. ENVIO PARA O HARDWARE ---

    // Passamos o buffer para a função do driver (que vai copiar para a fila DMA)
    bool result = hardware_tx_func(tx_buf, total_len);

    // --- 8. LIMPEZA ---

    // Como o hardware_tx_func faz sua própria cópia (malloc interno ou memcpy pro DMA),
    // nós somos responsáveis por liberar ESTE buffer temporário aqui.
    free(tx_buf);

    return result;
}

/**
 * @brief Decodifica e imprime o frame recebido para debug
 * @param packet Ponteiro para o pacote promiscuo recebido do driver
 */
void print_wifi_frame(wifi_promiscuous_pkt_t *packet)
{
    // 1. Acesso aos Metadados do Rádio (RSSI, Tamanho, etc)
    wifi_pkt_rx_ctrl_t *ctrl = &packet->rx_ctrl;

    // 2. Cast do Payload bruto para sua Union inteligente
    mac80211_frame *frame = (mac80211_frame *)packet->payload;

    // Cabeçalho Básico
    ESP_LOGI(TAG_DUMP, "=== WI-FI FRAME START ===");
    ESP_LOGI(TAG_DUMP, "PHY Info: RSSI=%d dBm, Rate=%d, Chan=%d, Len=%d",
             ctrl->rssi, ctrl->rate, ctrl->channel, ctrl->sig_len);

    // 3. Decodificando o Frame Control (Bit Fields)
    // Nota: Acesso via 'std' pois o cabeçalho é comum
    mac_frame_control_t *fc = &frame->std.mac_header.frame_control;

    ESP_LOGI(TAG_DUMP, "Header: Type=%d (0=Mgt,1=Ctrl,2=Data), Subtype=%d",
             fc->type, fc->subtype);

    ESP_LOGI(TAG_DUMP, "Flags: ToDS=%d, FromDS=%d, Retry=%d, PwrMgt=%d",
             fc->to_ds, fc->from_ds, fc->retry, fc->pwr_mgt);

    // 4. Endereços
    ESP_LOGI(TAG_DUMP, "Addrs: Dest=[" MAC_FMT "] Src=[" MAC_FMT "] BSS=[" MAC_FMT "]",
             MAC_ARG(frame->std.mac_header.receiver),
             MAC_ARG(frame->std.mac_header.transmitter),
             MAC_ARG(frame->std.mac_header.bssid));

    // 5. Lógica de Detecção de Action Frame Vendor Specific
    // Type 0 (Management) e Subtype 13 (Action)
    if (fc->type == 0 && fc->subtype == 13)
    {

        // Acessamos via máscara 'vendor' agora
        uint8_t category = frame->vendor.category;

        if (category == 127)
        {
            ESP_LOGW(TAG_DUMP, ">>> VENDOR SPECIFIC ACTION DETECTED <<<");
            ESP_LOGI(TAG_DUMP, "Category: %d (Vendor)", category);
            ESP_LOGI(TAG_DUMP, "OUI: %02X:%02X:%02X",
                     frame->vendor.oui[0], frame->vendor.oui[1], frame->vendor.oui[2]);
            ESP_LOGI(TAG_DUMP, "Protocol Version: 0x%02X", frame->vendor.oui_type);

            // Cálculo do Payload da Aplicação
            // Tamanho Total - (Header WiFi 24b + Vendor Header 5b + FCS 4b)
            int header_size = sizeof(wifi_mac_header_t) + 5; // 24 + 5 = 29 bytes
            int fcs_size = 4;
            int payload_len = ctrl->sig_len - header_size - fcs_size;

            if (payload_len > 0)
            {
                ESP_LOGI(TAG_DUMP, "OppNet Payload (%d bytes):", payload_len);
                ESP_LOG_BUFFER_HEX(TAG_DUMP, frame->vendor.payload, payload_len);
            }
            else
            {
                ESP_LOGW(TAG_DUMP, "Empty Payload");
            }
        }
        else
        {
            ESP_LOGI(TAG_DUMP, "Action Frame (Standard Category: %d)", category);
        }
    }
    else
    {
        ESP_LOGD(TAG_DUMP, "Not an Action Frame (Ignored for OppNet)");
    }

    ESP_LOGI(TAG_DUMP, "=== WI-FI FRAME END ===\n");
}
void open_mac_rx_callback(wifi_promiscuous_pkt_t *packet)
{
    // Gets called with a packet that was received. This function does not need to free the memory of the packet,
    //  but the packet will become invalid after this function returns. If you need any data from the packet,
    //  better copy it before returning!
    // Please avoid doing heavy processing here: it's not in an interrupt, but if this function is not fast enough,
    // the RX queue that is used to pass packets to this function might overflow and drop packets.
    print_wifi_frame(packet);
    if (packet->rx_ctrl.sig_len < 29)
    {
        return;
    }

    mac80211_frame *frame = (mac80211_frame *)packet->payload;
    // --- FILTRO NÍVEL 2: Frame Control (0x00D0) ---
    // Verifica se é Management (Type 0) e Action (Subtype 13)
    // 0x00D0 em Little Endian significa:
    // Byte 0: 0xD0 (Bits: Ver=00, Type=00, Subtype=1101 -> 13)
    // Byte 1: 0x00 (Flags zeradas)
    if (frame->raw_frame_control != 0x00D0)
    {
        return; // Não é Action Frame, tchau.
    }

    // --- FILTRO NÍVEL 3: Categoria Vendor (127) ---
    // Acessamos direto da memória do hardware. É seguro ler.
    if (frame->vendor.category != 127)
    {
        return; // É Action, mas não é Vendor (pode ser QoS, BlockAck, etc).
    }

    // --- FILTRO NÍVEL 4: OUI (Identidade) ---
    // Verifica se os 3 bytes de OUI batem com o nosso.
    // Isso garante que não pegaremos pacotes Vendor de outros fabricantes.
    if (memcmp(frame->vendor.oui, MY_OUI, 3) != 0)
    {
        return; // É Vendor, mas não é o nosso.
    }

    uint32_t total_len = packet->rx_ctrl.sig_len;
    uint8_t *safe_copy = malloc(total_len);

    if (safe_copy == NULL)
    {
        ESP_LOGE(TAG, "Sem memória para copiar pacote RX!");
        return;
    }

    // Copia do DMA para a Heap
    memcpy(safe_copy, packet->payload, total_len);

    // Prepara o item para a fila
    mac_rx_item_t item;
    item.buffer = safe_copy;
    item.len = total_len;
    item.rssi = packet->rx_ctrl.rssi;

    // Envia para a Task MAC (Core 1)
    if (xQueueSend(rx_queue_hw_to_mac, &item, 0) != pdTRUE)
    {
        // Se a fila encheu (a Task MAC está lenta), descartamos para não travar
        free(safe_copy);
        // Opcional: Contar estatística de packet drop
    }
}

void open_mac_tx_func_callback(tx_func *t)
{
    // This function will get called exactly once, with as argument a function (`bool tx_func(uint8_t* packet, uint32_t len)`).
    // The function that is passed will TX packets. If it returned `true`, that means that the packet was sent. If false,
    //  you'll need to call the function again.
    hardware_tx_func = t;
}

void send_link_ack(uint8_t *dest_mac, uint16_t seq_num_to_ack)
{
    if (!hardware_tx_func)
        return;

    // Tamanho: Header(24) + Vendor(5) = 29 bytes (Sem payload)
    uint16_t total_len = sizeof(wifi_mac_header_t) + 5;
    uint8_t *buf = malloc(total_len);
    if (!buf)
        return;

    vendor_action_frame_t *frame = (vendor_action_frame_t *)buf;
    memset(buf, 0, total_len);

    // Configura Header
    frame->mac_header.frame_control.type = 0;     // Mgt
    frame->mac_header.frame_control.subtype = 13; // Action
    memcpy(frame->mac_header.receiver, dest_mac, 6);
    memcpy(frame->mac_header.transmitter, module_mac_addr, 6);
    memcpy(frame->mac_header.bssid, dest_mac, 6);

    // Configura Vendor Action
    frame->category = 127;
    memcpy(frame->oui, MY_OUI, 3);
    frame->oui_type = 0x06; // TIPO 0x06 = ACK

    frame->mac_header.sequence_control.fragment_number = 0;
    frame->mac_header.sequence_control.sequence_number = seq_num_to_ack;

    hardware_tx_func(buf, total_len);
    free(buf);
}

// Função auxiliar para verificar duplicata
bool is_duplicate(uint8_t *src_mac, uint16_t seq_ctrl)
{
    for (int i = 0; i < MAX_NEIGHBORS; i++)
    {
        // Se achou o vizinho na cache
        if (memcmp(dedup_cache[i].mac, src_mac, 6) == 0)
        {
            // Se o numero de sequencia é o mesmo, é duplicata!
            if (dedup_cache[i].last_seq_ctrl == seq_ctrl)
            {
                return true;
            }
            // Atualiza o numero e retorna false (não é duplicata, é novo)
            dedup_cache[i].last_seq_ctrl = seq_ctrl;
            return false;
        }
    }
    // Se não achou (vizinho novo), adiciona na cache (lógica simplificada: overwrite index 0)
    // Numa implementação real, use LRU ou Ring Buffer
    static int next_idx = 0;
    memcpy(dedup_cache[next_idx].mac, src_mac, 6);
    dedup_cache[next_idx].last_seq_ctrl = seq_ctrl;
    next_idx = (next_idx + 1) % MAX_NEIGHBORS;

    return false;
}

void mac_rx_task(void *pvParam)
{
    mac_rx_item_t item;
    ESP_LOGI(TAG, "RX Task Iniciada (Modo Hardware ACK)");

    while (1)
    {
        if (xQueueReceive(rx_queue_hw_to_mac, &item, portMAX_DELAY))
        {
            mac80211_frame *frame = (mac80211_frame *)item.buffer;

            uint8_t *sender_mac = frame->vendor.mac_header.transmitter;
            uint8_t type = frame->vendor.oui_type;
            uint16_t seq = frame->vendor.mac_header.sequence_control.sequence_number;

            // Filtro de Duplicatas (Essencial pois o HW pode retransmitir)
            if (!is_duplicate(sender_mac, seq))
            {
                if (type == OPPNET_LINK_DATA)
                {
                    // NÃO ENVIAMOS ACK AQUI (Hardware já enviou)

                    ESP_LOGI(TAG, "RX Dados de " MAC_FMT " (Seq %d)", MAC_ARG(sender_mac), seq);

                    // Extrai Payload para a Aplicação
                    int overhead = sizeof(wifi_mac_header_t) + 5 + 4;
                    int payload_len = item.len - overhead;

                    if (payload_len > 0)
                    {
                        // Exemplo: xQueueSend(oppnet_queue, ...);
                        ESP_LOG_BUFFER_HEX("PAYLOAD", frame->vendor.payload, payload_len);
                    }
                }
                // Se recebermos um ACK (0x06), apenas ignoramos, pois usamos HW ACK agora.
            }
            else
            {
                // ESP_LOGW(TAG, "Duplicata Dropada Seq %d", seq);
            }

            free(item.buffer);
        }
    }
}

uint32_t calculate_backoff(uint8_t retry_count)
{

    // 1. Calcula o tamanho da janela (CW)
    // A cada retry, deslocamos 1 bit p/ esquerda (multiplica por 2)
    // Ex: retry 0 -> (15 << 0) = 15
    //     retry 1 -> (15 << 1) = 30
    //     retry 2 -> (15 << 2) = 60
    uint32_t cw = CW_MIN << retry_count;

    // 2. Trava no limite máximo (Clamp)
    if (cw > CW_MAX)
        cw = CW_MAX;

    // 3. Escolhe um slot aleatório dentro da janela [0 .. cw]
    // esp_random() retorna uint32_t. O operador % pega o resto da divisão.
    uint32_t random_slots = esp_random() % (cw + 1);

    // 4. Converte slots para tempo (us)
    return random_slots * SLOT_TIME_US;
}

void enviar_teste_unicast()
{
    // 1. Definir o Destino (O usuário passou 4 bytes, adicionei 00 00 para completar 6)
    // MAC Alvo: 24:DC:FF:54:00:00
    uint8_t target_mac[6] = {0x24, 0xDC, 0xc3, 0x98, 0x96, 0xb4}; // sem ena
    
    //uint8_t target_mac[6] = {0xcc, 0xdb, 0xa7, 0x1d, 0xfc, 0x1c};// com enable
    // 2. Preparar o Payload
    const char *mensagem = "Teste de Unicast OppNet";
    uint16_t len = strlen(mensagem);

    // 3. ALOCAR MEMÓRIA (Crucial! A mac_tx_task vai dar free nisso depois)
    uint8_t *payload_buffer = (uint8_t *)malloc(len);
    if (payload_buffer == NULL)
    {
        ESP_LOGE("APP", "Sem memoria para payload");
        return;
    }
    memcpy(payload_buffer, mensagem, len);

    // 4. Montar o Evento
    mac_event_t evt;
    evt.type = EVENT_TX_REQUEST; // Tipo: Pedido de Envio
    memcpy(evt.tx_req.dest_mac, target_mac, 6);
    evt.tx_req.payload = payload_buffer; // Ponteiro para a memória alocada
    evt.tx_req.len = len;

    // 5. Enviar para a fila da mac_tx_task
    if (mac_event_queue != NULL)
    {
        if (xQueueSend(mac_event_queue, &evt, pdMS_TO_TICKS(3000)))
        {
            ESP_LOGI("APP", "Pedido de envio colocado na fila!");
        }
        else
        {
            ESP_LOGE("APP", "Fila cheia! Envio descartado.");
            free(payload_buffer); // Se não entrou na fila, nós limpamos aqui para não vazar
        }
    }
    else
    {
        ESP_LOGE("APP", "Erro: Fila mac_event_queue não inicializada.");
        free(payload_buffer);
    }
}

void mac_tx_task(void *pvParam)
{
    mac_event_t evt;
    mac_event_queue = xQueueCreate(20, sizeof(mac_event_t));

    static uint16_t global_seq_num = 0;

    // Buffer Estático para o Hello (Para não dar malloc toda vez)
    // Tamanho: Header(24) + Vendor(5) + Payload(1) = 30 bytes
    static uint8_t hello_buffer[32];
    vendor_action_frame_t *hello_frame = (vendor_action_frame_t *)hello_buffer;

    // Preparação Única do Hello (Zero Copy na execução)
    memset(hello_buffer, 0, sizeof(hello_buffer));

    // Configura cabeçalhos fixos do Hello
    hello_frame->mac_header.frame_control.type = 0;     // Mgt
    hello_frame->mac_header.frame_control.subtype = 13; // Action
    // Destino Broadcast
    memset(hello_frame->mac_header.receiver, 0xFF, 6);
    // BSSID Privado
    memcpy(hello_frame->mac_header.bssid, bssid_mac_addr, 6);
    // Source e OUI
    memcpy(hello_frame->mac_header.transmitter, module_mac_addr, 6);
    hello_frame->category = 127;
    memcpy(hello_frame->oui, MY_OUI, 3);
    hello_frame->oui_type = 0x01;   // Tipo Interno HELLO
    hello_frame->payload[0] = 0x01; // Payload HELLO

    ESP_LOGI(TAG, "TX Task Unificada (App + Feedback + Hello)");

    // Inicia o Timer pela primeira vez
    xTimerStart(hello_timer, 0);

    while (1)
    {
        // Bloqueia esperando App pedir envio OU Hardware confirmar
        if (xQueueReceive(mac_event_queue, &evt, portMAX_DELAY))
        {
            switch (evt.type)
            {
            // --- CASO 1: ENVIAR NOVO PACOTE ---
            case EVENT_TX_REQUEST:
            {
                uint16_t my_seq = global_seq_num;

                // IMPORTANTE: build_and_send_frame DEVE gravar 'my_seq' no pacote
                // para que o hardware consiga ler depois.
                bool sent = build_and_send_frame(
                    evt.tx_req.dest_mac,
                    evt.tx_req.payload,
                    evt.tx_req.len,
                    my_seq);

                if (sent)
                {
                    // ESP_LOGI(TAG, "--> Enviando Seq %d...", my_seq);
                    global_seq_num = (global_seq_num + 1) & 0xFFF;
                }
                else
                {
                    ESP_LOGE(TAG, "Erro ao enviar Seq %d (HW Cheio)", my_seq);
                }

                // Libera payload da aplicação
                if (evt.tx_req.payload)
                    free(evt.tx_req.payload);
                break;
            }

            // --- CASO 2: FEEDBACK DO HARDWARE ---
            case EVENT_TX_COMPLETE:
            {
                if (evt.tx_conf.success)
                {
                    ESP_LOGI(TAG, "[V] SUCESSO: Seq %d confirmado (ACK)", evt.tx_conf.seq_num);
                    // AQUI: Estatísticas++, Notificar App, etc.
                }
                else
                {
                    ESP_LOGW(TAG, "[X] FALHA: Seq %d (Timeout/Sem ACK)", evt.tx_conf.seq_num);
                    // AQUI: Lógica de reenvio ou remover vizinho
                }
                break;
            }
            case EVENT_TX_HELLO:
            {
                // Atualiza apenas o Sequence Number no buffer estático
                hello_frame->mac_header.sequence_control.sequence_number = global_seq_num;

                // Envia direto para o hardware (hardware_tx_func)
                // Usamos a função crua para evitar mallocs desnecessários
                if (hardware_tx_func)
                {
                    // hardware_tx_func(hello_buffer, 30); // 30 bytes fixos
                    // ESP_LOGI(TAG, "Hello Broadcast Seq %d", global_seq_num);
                    enviar_teste_unicast();
                }

                // Incrementa Seq Global
                global_seq_num = (global_seq_num + 1) & 0xFFF;

                // --- CÁLCULO DE JITTER (IMPORTANTE) ---
                // Para evitar colisão, o próximo Hello deve ser aleatório.
                // Intervalo: 1000ms +/- 200ms (800ms a 1200ms)
                int next_interval_ms = hello_interval_ms + ((esp_random() % 401) - 200);

                // Reprograma o timer para o novo tempo
                xTimerChangePeriod(hello_timer, pdMS_TO_TICKS(next_interval_ms), 0);
                break;
            }
            }
        }
    }
}

void hello_timer_callback(TimerHandle_t xTimer)
{
    mac_event_t evt;
    evt.type = EVENT_TX_HELLO;
    xQueueSend(mac_event_queue, &evt, 0);
}

void IRAM_ATTR my_tx_done_callback(uint16_t seq, bool success)
{
    mac_event_t evt;
    evt.type = EVENT_TX_COMPLETE;
    evt.tx_conf.seq_num = seq;
    evt.tx_conf.success = success;

    // Agora sim, O MAC sabe qual fila usar
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(mac_event_queue, &evt, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

void mac_init()
{
    // Cria filas e semáforos
    rx_queue_hw_to_mac = xQueueCreate(10, sizeof(mac_rx_item_t));
    hello_timer = xTimerCreate("HelloTimer", pdMS_TO_TICKS(1000), pdFALSE, NULL, hello_timer_callback);
    // Cria as Tasks no Core 1

    hardware_register_tx_callback(my_tx_done_callback);
    xTaskCreatePinnedToCore(mac_rx_task, "MAC_RX", 4098, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(mac_tx_task, "MAC_TX", 4098, NULL, 4, NULL, 1);
}
