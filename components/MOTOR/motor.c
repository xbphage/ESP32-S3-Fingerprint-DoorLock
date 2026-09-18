#include "motor.h"
#include "pwm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


void set_angle(uint16_t angle){
    if (angle > 270){
        angle=0;
    }
    uint16_t duty = ((float)angle / 2700 + 0.025 ) * 1024;
    set_duty(duty);
}

void open_door(void){
    set_angle(180);
    vTaskDelay(pdMS_TO_TICKS(5000));
    set_angle(0);
}