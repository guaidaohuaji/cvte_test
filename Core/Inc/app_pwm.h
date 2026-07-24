#ifndef APP_PWM_H
#define APP_PWM_H

#include <stdbool.h>
#include <stdint.h>

bool AppPwm_Init(void);
bool AppPwm_SetFrequency(uint32_t frequency_hz);
bool AppPwm_SetDutyX100(uint16_t duty_x100);
bool AppPwm_Enable(bool enabled);
bool AppPwm_IsEnabled(void);

uint32_t AppPwm_GetTargetFrequency(void);
uint32_t AppPwm_GetActualFrequency(void);
int32_t  AppPwm_GetFrequencyErrorPpm(void);

uint16_t AppPwm_GetTargetDutyX100(void);
uint16_t AppPwm_GetActualDutyX100(void);

uint32_t AppPwm_GetPrescaler(void);
uint32_t AppPwm_GetAutoReload(void);
uint32_t AppPwm_GetCompare(void);

#endif
