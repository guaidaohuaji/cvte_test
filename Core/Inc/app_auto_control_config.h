#ifndef APP_AUTO_CONTROL_CONFIG_H
#define APP_AUTO_CONTROL_CONFIG_H

#include "app_onewire_config.h"
#include "app_fan_config.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
#define APP_AUTO_CONTROL_ENABLED  1
#else
#define APP_AUTO_CONTROL_ENABLED  0
#endif

#define APP_AUTO_CONTROL_PERIOD_MS            1000U

#define APP_AUTO_TEMP_MIN_CENTI_C            (-2500)
#define APP_AUTO_TEMP_MAX_CENTI_C             6000

#define APP_AUTO_FAN_MIN_RPM                  1000U
#define APP_AUTO_FAN_MAX_RPM                  2300U

#define APP_AUTO_DAMPER_COLD_STEPS            1700
#define APP_AUTO_DAMPER_HOT_STEPS                0

#define APP_AUTO_TEMP_STARTUP_GRACE_MS        1000U

#define APP_AUTO_DEFAULT_MODE_MANUAL             1U

#define APP_AUTO_FAN_CONTROL_PERIOD_MS        1000U
#define APP_AUTO_FAN_TOLERANCE_RPM             100U
#define APP_AUTO_FAN_PWM_STEP_X100             100U
#define APP_AUTO_FAN_INITIAL_DUTY_X100        3000U
#define APP_AUTO_FAN_FAILSAFE_DUTY_X100      10000U

#define APP_AUTO_DAMPER_CONTROL_PERIOD_MS     1000U
#define APP_AUTO_DAMPER_DEADBAND_STEPS          20U
#define APP_AUTO_DAMPER_FAILSAFE_STEPS            0
#define APP_AUTO_DAMPER_STOP_ON_MANUAL           1U

#endif
