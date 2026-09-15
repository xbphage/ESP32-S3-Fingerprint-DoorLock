#ifndef __MOTOR_H
#define __MOTOR_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void set_angle(uint16_t angle);
void open_door(void);

#ifdef __cplusplus
}
#endif

#endif
