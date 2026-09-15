#include "pwm.h"
#include "driver/ledc.h"

void pwm_init(void)
{
    ledc_timer_config_t ledctimer_structure = {
        .clk_cfg = LEDC_AUTO_CLK,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 50,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
    };
    ledc_timer_config(&ledctimer_structure);

    ledc_channel_config_t   ledcchannel_structure = {
        .channel = LEDC_CHANNEL_1,
        .duty = 512,
        .flags.output_invert = 0,
        // ESP32-C3 上 GPIO8 是 strapping 脚，官方 DevKitM-1 还在这上面焊了 RGB LED，
        // 拿来做 PWM 会一直闪灯，所以舵机信号挪到 GPIO6。
        .gpio_num = GPIO_NUM_6,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_1,
    };
    ledc_channel_config(&ledcchannel_structure);
}

void set_duty(uint16_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty); 
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);        
}