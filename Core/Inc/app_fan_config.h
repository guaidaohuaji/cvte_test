#ifndef APP_FAN_CONFIG_H
#define APP_FAN_CONFIG_H

#define APP_FAN_PWM_FREQUENCY_HZ        10000U
#define APP_FAN_PWM_TIMER_CLOCK_HZ      168000000UL
#define APP_FAN_PWM_PRESCALER           167U
#define APP_FAN_PWM_PERIOD              99U

#define APP_FAN_STARTUP_BOOST_MS        5000U

#define APP_FAN_MIN_DUTY_X100           1000U
#define APP_FAN_MAX_DUTY_X100           10000U

#define APP_FAN_RPM_FACTOR              30U
#define APP_FAN_NO_TACH_TIMEOUT_MS      500U

#endif
