#ifndef __PWM_H_
#define __PWM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pwm_init(void);
void set_duty(uint16_t duty);

#ifdef __cplusplus
}
#endif

#endif
