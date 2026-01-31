// current_connections *create_list_current_connections()
// {
//     return NULL;
// }
// payload_mensages *create_list_payload_mensages()
// {
//     return NULL;
// }

// current_connections *insert_current_connections(current_connections *head, uint8_t mac[6], uint8_t state)
// {
//     current_connections *new_conection = malloc(sizeof(current_connections));
//     current_connections *item = head;

//     memcpy(new_conection->mac_adress, mac, 6);
//     new_conection->status = state;
//     new_conection->next = NULL;
//     new_conection->communication_attempts = 0;
//     // new_conection->last_transmission = 0;
//     new_conection->communication_state = WAIT;
//     new_conection->wait_time_to_send = 0;
//     // while (new_conection->wait_time_to_send > 750 || new_conection->wait_time_to_send < 500)
//     // {
//     //     esp_fill_random(&new_conection->wait_time_to_send, 2);
//     // }

//     if (head == NULL)
//     {
//         return new_conection;
//     }

//     while (item->next != NULL)
//     {
//         item = item->next;
//     }
//     item->next = new_conection;

//     return head;
// }
// payload_mensages *create_payload_mensage(payload_mensages *head, uint8_t mac[6], uint8_t *mensage_to_send, uint8_t size_of_mensage)
// {
//     payload_mensages *new_mensage = malloc(sizeof(payload_mensages));
//     payload_mensages *item = head;
//     uint8_t *path = malloc(7);
//     memcpy(path, module_mac_addr, 6);
//     path[7] = '/';

//     new_mensage->id = 0x65;
//     // new_mensage->time_create = esp_timer_get_time();
//     memcpy(new_mensage->destination_mac, mac, 6);
//     new_mensage->size_of_data = size_of_mensage; // size max 1500 bytes -mac frame header
//     new_mensage->size_of_path = 7;

//     new_mensage->data = mensage_to_send;

//     new_mensage->path_mensage = path; // vai virar uma nova estrutura       MAC1/MAC2
//     new_mensage->next = NULL;
//     new_mensage->size_packet = new_mensage->size_of_data + new_mensage->size_of_path + 10;
//     if (head == NULL)
//     {
//         return new_mensage;
//     }

//     while (item->next != NULL)
//     {
//         item = item->next;
//     }
//     item->next = new_mensage;

//     return head;
// }
// payload_mensages *insert_payload_mensage(payload_mensages *head, uint8_t *mensage_received)
// {
//     payload_mensages *new_mensage = malloc(sizeof(payload_mensages));
//     payload_mensages *item = head;

//     memcpy(&new_mensage->id, mensage_received, 1);
//     // memcpy(&new_mensage->time_create, 1 + mensage_received, 8);
//     memcpy(new_mensage->destination_mac, 1 + mensage_received, 6);
//     memcpy(&new_mensage->size_of_data, 7 + mensage_received, 1);
//     memcpy(&new_mensage->size_of_path, 8 + mensage_received, 1);
//     // memcpy(&new_mensage->size_packet, 9 + mensage_received, 1);
//     new_mensage->size_packet = 0x41;
//     ESP_LOGW(TAG, "tamnho mensagem : %x", new_mensage->size_of_data);
//     ESP_LOGW(TAG, "caminho : %x", new_mensage->size_of_path);
//     ESP_LOGW(TAG, "tamanho pacote : %x", new_mensage->size_packet);
//     uint8_t *mensage_data_received = malloc(new_mensage->size_of_data);
//     uint8_t *mensage_path_received = malloc(new_mensage->size_of_path);

//     memcpy(mensage_data_received, 10 + mensage_received, new_mensage->size_of_data);
//     memcpy(mensage_path_received, (10 + new_mensage->size_of_data) + mensage_received, new_mensage->size_of_path);

//     new_mensage->data = mensage_data_received;
//     new_mensage->path_mensage = mensage_data_received;
//     new_mensage->next = NULL;

//     if (head == NULL)
//     {
//         return new_mensage;
//     }

//     while (item->next != NULL)
//     {
//         item = item->next;
//     }
//     item->next = new_mensage;
//     ESP_LOGW(TAG, "NOVA CONEXAO CRIADA");
//     return head;
// }
// payload_mensages *remove_mensage(payload_mensages **head, payload_mensages **connection_removed)
// {
//     payload_mensages *item = *head;
//     payload_mensages *previous_item = NULL;
//     ESP_LOGD(TAG, "APAGANDO A CONEXÃO");
//     while (item != NULL && item != *connection_removed)
//     {
//         previous_item = item;
//         item = item->next;
//     }
//     free(item->data);
//     free(item->path_mensage);
//     if (item != NULL)
//     {
//         if (previous_item == NULL)
//         {
//             *head = item->next;
//             previous_item = item->next;
//         }
//         else
//         {
//             previous_item->next = item->next;
//         }
//         free(item);
//     }
//     return previous_item;
// }

// current_connections *list_connections;
// payload_mensages *list_mensages;

// void update_conection(current_connections *connection, uint8_t state)
// {
//     connection->status = state;
//     connection->communication_attempts = 0;
//     connection->wait_time_to_send = 0;
// }
// void update_communication_state(current_connections *connection, communication_stages state)
// {
//     connection->communication_state = state;
//     connection->communication_attempts = 0;
// }

// current_connections *search_conection(current_connections *head, uint8_t mac[6])
// {
//     current_connections *item = head;
//     while (item != NULL && memcmp(item->mac_adress, mac, 6))
//     {
//         item = item->next;
//     }
//     return item;
// }

// current_connections *remove_conection(current_connections **head, current_connections **connection_removed)
// {
//     current_connections *item = *head;
//     current_connections *previous_item = NULL;
//     ESP_LOGD(TAG, "APAGANDO A CONEXÃO");
//     while (item != NULL && item != *connection_removed)
//     {
//         previous_item = item;
//         item = item->next;
//     }

//     if (item != NULL)
//     {
//         if (previous_item == NULL)
//         {
//             *head = item->next;
//             previous_item = item->next;
//         }
//         else
//         {
//             previous_item->next = item->next;
//         }
//         free(item);
//     }
//     return previous_item;
// }

// void send_mensages(current_connections *connection)
// {
//     // 0
//     // 0x31 dados
//     // 0x32 confirmacao
//     // 0x33 esperando
//     // 0x34 sem dados
//     uint8_t mac_frame_data[30];
//     switch (connection->communication_state)
//     {

//     case SEND_MESSAGE_DESTINATIONS:
//         if (list_mensages != NULL)
//         {
//             ESP_LOGW(TAG, "enviadno mensagem");
//             connection->communication_state = SEND_MESSAGE_DESTINATIONS;

//             uint16_t size_packet_to_send = 29 + list_mensages->size_packet;
//             uint16_t size_mensage_and_header = 35 + list_mensages->size_of_data;
//             uint8_t mac_frame_data2[size_packet_to_send];

//             memcpy(mac_frame_data2, mac_frame_header_template, 24);
//             mac_frame_data2[24] = 0x31;

//             memcpy(&mac_frame_data2[25], list_mensages, 10);
//             memcpy(&mac_frame_data2[35], list_mensages->data, list_mensages->size_of_data);
//             memcpy(&mac_frame_data2[size_mensage_and_header], list_mensages->path_mensage, list_mensages->size_of_path);

//             for (int i = 0; i < 2; i++)
//             {
//                 mac_frame_data2[size_packet_to_send - i] = 0xFF;
//             }

//             set_addresses(mac_frame_data2, IEEE80211_ACK, connection->mac_adress);
//             tx(mac_frame_data2, sizeof(mac_frame_data2));
//         }
//         else
//         {
//             memcpy(mac_frame_data, mac_frame_header_template, 24);
//             mac_frame_data[24] = 0x34;

//             for (int i = 0; i < 4; i++)
//             {
//                 mac_frame_data[28 - i] = 0xFF;
//             }

//             set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
//             tx(mac_frame_data, sizeof(mac_frame_data));
//         }
//         break;

//     case COMPLETE:

//         memcpy(mac_frame_data, mac_frame_header_template, 24);
//         mac_frame_data[24] = 0x32;

//         for (int i = 0; i < 4; i++)
//         {
//             mac_frame_data[28 - i] = 0xFF;
//         }

//         set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
//         tx(mac_frame_data, sizeof(mac_frame_data));
//         // connection->wait_time_to_send = 5000;
//         break;

//     default:
//         memcpy(mac_frame_data, mac_frame_header_template, 24);
//         mac_frame_data[24] = 0x33;

//         for (int i = 0; i < 4; i++)
//         {
//             mac_frame_data[28 - i] = 0xFF;
//         }

//         set_addresses(mac_frame_data, IEEE80211_ACK, connection->mac_adress);
//         tx(mac_frame_data, sizeof(mac_frame_data));
//         break;
//     }
// }

// int search_mac(current_connections *head, uint8_t mac[6])
// {
//     current_connections *item = head;
//     if (item != NULL)
//     {
//         while (memcmp(item->mac_adress, mac, 6) && item != NULL)
//         {
//             item = item->next;
//         }
//         if (item != NULL)
//         {
//             return true;
//         }
//     }
//     return false;
// }