#ifndef APP_LED_H
#define APP_LED_H

#include <stdbool.h>

typedef enum
{
    APP_LED_MODE_AUTO = 0,
    APP_LED_MODE_MANUAL = 1
} AppLedMode;

void AppLed_Init(void);
void AppLed_Process(void);

void AppLed_SetAutomatic(void);
void AppLed_SetManual(bool on);

AppLedMode AppLed_GetMode(void);
bool AppLed_IsOn(void);

#endif
