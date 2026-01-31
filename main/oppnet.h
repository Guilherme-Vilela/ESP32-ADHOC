// typedef enum
// {
//     WAIT,
//     SEND_MESSAGE_DESTINATIONS,
//     SEND_MESSAGE_DESTINATIONS_INTEREST,
//     SEND_REQUESTED_MESSAGES_INTEREST,
//     SEND_REQUESTED_MESSAGES,
//     COMPLETE,
//     SEND_1,
//     SEND_2
// } communication_stages;

// typedef struct current_connections
// {
//     uint8_t mac_adress[6];
//     openmac_sta_state_t status;
//     uint8_t communication_attempts;
//     uint8_t wait_time_to_send;
//     communication_stages communication_state;
//     struct current_connections *next;
// } current_connections;

// typedef struct payload_mensages
// {
//     uint8_t id; // 0
//     // uint64_t time_create;
//     uint8_t destination_mac[6]; // 1-6
//     uint8_t size_of_data;       //        //7
//     uint8_t size_of_path;       //        //8
//     uint8_t size_packet;        // 9
//     uint8_t *data;              // 11 -
//     uint8_t *path_mensage;      // vai virar uma nova estrutura       MAC-TIME/MAC-TIME
//     struct payload_mensages *next;

// } payload_mensages;

// extern SemaphoreHandle_t timer_send_mensage_semaphore;
// #define time_wait_alarm_send_mensage 10000 // counter of clock
// #define wait_time_max_probe_request 10     // ticks of de alarm clock
// #define wait_time_max_resend_mensage 5     // ticks of alarm clock
// #define frequency_timer 100000             // 100kHz
// #define max_attempts 10                    // 10 tentativas
// #define NOME_ESP "Ola sou NO 2"
// #define ID_ESP 2