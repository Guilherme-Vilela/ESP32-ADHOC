#include <string.h>
#include <stdlib.h>

#include "esp_system.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_phy_init.h" // for get_phy_version_str

#include "esp_mac.h" // pegar endereço mac original
#include "driver/gpio.h"
#include "hardware.h"
#include "mac.h"

#include "driver/gptimer.h"
// #include <time.h>

#ifndef CONFIG_IDF_TARGET_ESP32
#error "This uses low-level hardware peripherals and hardcoded addresses, it is only tested on the plain ESP32 for now"
#endif

#if (!(ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR == 0 && ESP_IDF_VERSION_PATCH == 1))
#error "This project currently still uses the proprietary wifi library for initialization, this was only tested with ESP-IDF v5.0.1"
#endif

hardware_mac_args open_hw_args = {
	._rx_callback = open_mac_rx_callback,
	._tx_func_callback = open_mac_tx_func_callback};

// void print_mac(uint8_t * mac){
//     int i;
//     printf("\n\n\n\n MAC é ");
//     for(i=0;i<6;i++){
//         printf("%x", mac[i]);
//     }
//     printf("\n");
// }

void get_mac()
{
	esp_err_t ret = ESP_OK;
	ret = esp_read_mac(module_mac_addr, ESP_MAC_WIFI_STA);
	// print_mac(module_mac_addr);
}

static bool IRAM_ATTR timer_send_mensages(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data)
{
	BaseType_t high_task_awoken = pdFALSE;
	xSemaphoreGiveFromISR(timer_send_mensage_semaphore, &high_task_awoken);
	return pdFALSE;
}
void timer_init()
{
	char *TAG_timer = "TIMER_SEMAPHORE";
	timer_send_mensage_semaphore = xSemaphoreCreateBinary();
	if (timer_send_mensage_semaphore == NULL)
	{
		ESP_LOGE(TAG_timer, "Falha ao criar o semáforo");
	}

	ESP_LOGI(TAG_timer, "Criando Semaforo");

	gptimer_handle_t gptimer = NULL;
	gptimer_config_t timer_config = {
		.clk_src = GPTIMER_CLK_SRC_DEFAULT,
		.direction = GPTIMER_COUNT_UP,
		.resolution_hz = frequency_timer, // 
	};
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

	gptimer_event_callbacks_t cbs = {
		.on_alarm = timer_send_mensages,
	};
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, 0));

	ESP_LOGI(TAG_timer, "Habiltiando o timer");
	ESP_ERROR_CHECK(gptimer_enable(gptimer));

	ESP_LOGI(TAG_timer, "criando alarme par a o timer");
	gptimer_alarm_config_t alarm_config1 = {
		.reload_count = 0,
		.flags.auto_reload_on_alarm = true,
		.alarm_count = time_wait_alarm_send_mensage, // period = 1s
	};
	ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config1));
	ESP_ERROR_CHECK(gptimer_start(gptimer));
}

void app_main(void)
{
	gpio_reset_pin(19);
	gpio_reset_pin(21);
	gpio_set_direction(19, GPIO_MODE_OUTPUT);
	gpio_set_direction(21, GPIO_MODE_OUTPUT);
	gpio_set_level(21, 1);

	// time_t now;
	// char strftime_buf[64];
	// struct tm timeinfo;

	// time(&now);
	// // Set timezone to China Standard Time
	// setenv("TZ", "CST-8", 1);
	// tzset();

	// localtime_r(&now, &timeinfo);
	// strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
	// ESP_LOGI("teste", "The current date/time in Shanghai is: %s", strftime_buf);

	timer_init();

	const char *actual_version_string = get_phy_version_str();
	const char *expected_version_string = "4670,719f9f6,Feb 18 2021,17:07:07";
	if (strcmp(expected_version_string, actual_version_string) != 0)
	{
		ESP_LOGE("main", "get_phy_version_str() wrong: is '%s' but should be '%s'", actual_version_string, expected_version_string);
		abort();
	}

	get_mac();
	xTaskCreatePinnedToCore(&mac_task, "open_mac", 4096, NULL, /*prio*/ 3, NULL, /*core*/ 1);
	xTaskCreatePinnedToCore(&mac_send_package, "mac_send_mensages", 2054, NULL, /*prio*/ 1, NULL, /*core*/ 1);
	xTaskCreatePinnedToCore(&wifi_hardware_task, "wifi_hardware", 4096, &open_hw_args, /*prio*/ 5, NULL, /*core*/ 0);
}
