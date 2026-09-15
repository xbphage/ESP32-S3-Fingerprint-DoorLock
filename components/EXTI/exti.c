#include "exti.h"
#include "driver/gpio.h"
#include <stdint.h>
#include "driver/gptimer.h"
#include <esp_timer.h>
uint32_t i = 0;

static void i_pro(void *args){
    i++;
    gpio_intr_enable(GPIO_NUM_9);
}

esp_timer_create_args_t args={
    .arg = NULL,
    .callback = i_pro,
    .name = "i_pro",
};

static esp_timer_handle_t out_handle = NULL;

void exti_isr_handler_9(void *arg){
    gpio_intr_disable(GPIO_NUM_9);
    esp_timer_start_once(out_handle,100*1000);
}

uint32_t get_i(){
    return i;
}

void exti_init(){
    gpio_config_t pGPIOConfig = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ull << GPIO_NUM_9,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&pGPIOConfig);

    gpio_install_isr_service(ESP_INTR_FLAG_EDGE);

    gpio_isr_handler_add(GPIO_NUM_9, exti_isr_handler_9, NULL);

    gpio_intr_enable(GPIO_NUM_9);

    esp_timer_create(&args, &out_handle);
}