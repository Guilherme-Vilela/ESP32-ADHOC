
#include <stdint.h>

typedef struct data_dtn
{
    uint64_t time;
    uint8_t  destination_mac[6];
    uint8_t *data;
    uint8_t size_of_data; 

    uint8_t path; // vai virar uma nova estrutura

} data_dtn;

