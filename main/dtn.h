
#include <stdint.h>

typedef struct data_dtn
{
    uint64_t time_create;
    uint8_t  destination_mac[6];
    uint8_t *data;        
    uint8_t size_of_data; //size max 1500bits            
    uint8_t *path_mensage; // vai virar uma nova estrutura       MAC-TIME/MAC-TIME
    uint8_t size_of_path; //size max 1500bits
} data_dtn;

