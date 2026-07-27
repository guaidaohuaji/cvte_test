#ifndef APP_DAMPER_CONFIG_H
#define APP_DAMPER_CONFIG_H

#include "app_onewire_config.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
#define APP_DAMPER_ENABLED  1
#else
#define APP_DAMPER_ENABLED  0
#endif

#define APP_DAMPER_FULL_TRAVEL_STEPS       1700U
#define APP_DAMPER_STEP_PPS                 300U
#define APP_DAMPER_DIAG_MAX_RELATIVE_STEPS  100U
#define APP_DAMPER_POST_MOVE_HOLD_MS        100U
#define APP_DAMPER_FORWARD_IS_OPEN            0U  /* 实物确认: 正向=关闭 */

#define APP_DAMPER_TIMER_CLOCK_HZ       84000000UL
#define APP_DAMPER_TIM_PSC                   279U
#define APP_DAMPER_TIM_ARR                   999U

#endif
