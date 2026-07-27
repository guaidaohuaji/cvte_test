#include "app_damper.h"
#include "app_onewire_config.h"
#include "main.h"
#include "tim.h"

#include <stddef.h>

typedef enum {
    DAMPER_PHASE_0 = 0,
    DAMPER_PHASE_1,
    DAMPER_PHASE_2,
    DAMPER_PHASE_3,
    DAMPER_PHASE_COUNT
} DamperPhaseIndex;

typedef struct {
    uint32_t b_bsrr;
    uint32_t c_bsrr;
} DamperPhaseBsrr;

static const DamperPhaseBsrr phase_bsrr[DAMPER_PHASE_COUNT] = {
    /* Phase 0: PB3=1 PB4=0 PB5=1 PC14=0 */
    { (GPIO_PIN_3 | GPIO_PIN_5) | ((uint32_t)GPIO_PIN_4 << 16U),
      ((uint32_t)GPIO_PIN_14 << 16U) },
    /* Phase 1: PB3=0 PB4=1 PB5=1 PC14=0 */
    { (GPIO_PIN_4 | GPIO_PIN_5) | ((uint32_t)GPIO_PIN_3 << 16U),
      ((uint32_t)GPIO_PIN_14 << 16U) },
    /* Phase 2: PB3=0 PB4=1 PB5=0 PC14=1 */
    { GPIO_PIN_4 | (((uint32_t)GPIO_PIN_3 | (uint32_t)GPIO_PIN_5) << 16U),
      GPIO_PIN_14 },
    /* Phase 3: PB3=1 PB4=0 PB5=0 PC14=1 */
    { GPIO_PIN_3 | (((uint32_t)GPIO_PIN_4 | (uint32_t)GPIO_PIN_5) << 16U),
      GPIO_PIN_14 }
};

static volatile DamperState state;
static volatile bool position_valid;
static volatile int32_t current_steps;
static volatile int32_t target_steps;
static volatile uint32_t remaining_steps;
static volatile uint8_t phase_index;
static volatile bool phase_energized;
static volatile DamperCommand last_command;
static volatile DamperResult last_result;
static volatile uint8_t fault_flags;
static volatile bool released;
static volatile bool moving_forward;
static volatile uint32_t hold_start_tick;

static bool gpio_initialized;
static bool tim6_initialized;
static bool init_ok;

__attribute__((unused))
static void damper_gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5,
                      GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_14 | GPIO_PIN_15,
                      GPIO_PIN_RESET);

    {
        GPIO_InitTypeDef init_cfg = {0};
        init_cfg.Pin  = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
        init_cfg.Mode = GPIO_MODE_OUTPUT_PP;
        init_cfg.Pull = GPIO_NOPULL;
        init_cfg.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOB, &init_cfg);
    }

    {
        GPIO_InitTypeDef init_cfg = {0};
        init_cfg.Pin  = GPIO_PIN_14 | GPIO_PIN_15;
        init_cfg.Mode = GPIO_MODE_OUTPUT_PP;
        init_cfg.Pull = GPIO_NOPULL;
        init_cfg.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOC, &init_cfg);
    }

    gpio_initialized = true;
}

static void damper_write_phase(DamperPhaseIndex idx)
{
    const DamperPhaseBsrr *p = &phase_bsrr[idx];
    GPIOB->BSRR = p->b_bsrr;
    GPIOC->BSRR = p->c_bsrr;
}

static void damper_set_standby(bool on)
{
    if (on)
    {
        GPIOC->BSRR = GPIO_PIN_15;
    }
    else
    {
        GPIOC->BSRR = (uint32_t)GPIO_PIN_15 << 16U;
    }
}

static void damper_clear_outputs(void)
{
    GPIOB->BSRR = ((uint32_t)(GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5)) << 16U;
    GPIOC->BSRR = (uint32_t)GPIO_PIN_14 << 16U;
}

static void damper_tim6_reset(void)
{
    if (!tim6_initialized) return;

    __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
    __HAL_TIM_DISABLE(&htim6);
    __HAL_TIM_SET_COUNTER(&htim6, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
    HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
}

__attribute__((unused))
static void damper_enter_fault(void)
{
    if (tim6_initialized)
    {
        damper_tim6_reset();
    }

    if (gpio_initialized)
    {
        damper_set_standby(false);
        damper_clear_outputs();
    }

    state            = DAMPER_STATE_FAULT;
    released         = true;
    position_valid   = false;
    remaining_steps  = 0U;
    last_result      = DAMPER_RESULT_HARDWARE_ERROR;
    fault_flags     |= 0x02U;
}

static bool damper_is_moving(void)
{
    return (state == DAMPER_STATE_MOVING_FORWARD ||
            state == DAMPER_STATE_MOVING_REVERSE);
}

__attribute__((unused))
static bool damper_is_busy(void)
{
    return damper_is_moving() || (state == DAMPER_STATE_POST_MOVE_HOLD);
}

/* Startup: restore saved phase without counting, then enable STBY */
__attribute__((unused))
static void damper_start_motion(uint32_t steps, bool forward)
{
    damper_tim6_reset();

    remaining_steps = steps;
    moving_forward  = forward;
    state           = forward ? DAMPER_STATE_MOVING_FORWARD
                              : DAMPER_STATE_MOVING_REVERSE;
    last_result     = DAMPER_RESULT_IN_PROGRESS;
    released        = false;

    /* Phase 0-energize: output current phase with STBY low */
    damper_write_phase((DamperPhaseIndex)(phase_index & 0x03U));
    phase_energized = true;

    /* Pull STBY high */
    damper_set_standby(true);

    /* Confirm flags clean, then start */
    __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
    __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
    __HAL_TIM_ENABLE(&htim6);
}

void AppDamper_Init(void)
{
#if APP_DAMPER_ENABLED
    state            = DAMPER_STATE_POSITION_UNKNOWN;
    position_valid   = false;
    current_steps    = 0;
    target_steps     = 0;
    remaining_steps  = 0U;
    phase_index      = 0U;
    phase_energized  = false;
    last_command     = DAMPER_CMD_NONE;
    last_result      = DAMPER_RESULT_SUCCESS;
    fault_flags      = 0x00U;
    released         = true;
    moving_forward   = false;
    hold_start_tick  = 0U;
    gpio_initialized = false;
    tim6_initialized = false;
    init_ok          = true;

    damper_gpio_init();

    if (htim6.Instance == TIM6)
    {
        tim6_initialized = true;
    }
#else
    state            = DAMPER_STATE_UNAVAILABLE;
    position_valid   = false;
    current_steps    = 0;
    target_steps     = 0;
    remaining_steps  = 0U;
    phase_index      = 0U;
    phase_energized  = false;
    last_command     = DAMPER_CMD_NONE;
    last_result      = DAMPER_RESULT_SUCCESS;
    fault_flags      = 0x00U;
    released         = true;
    moving_forward   = false;
    hold_start_tick  = 0U;
    gpio_initialized = false;
    tim6_initialized = false;
    init_ok          = false;
#endif
}

void AppDamper_Process(void)
{
    if (!init_ok) return;

    if (state == DAMPER_STATE_POST_MOVE_HOLD)
    {
        if ((uint32_t)(HAL_GetTick() - hold_start_tick) >=
            APP_DAMPER_POST_MOVE_HOLD_MS)
        {
            uint32_t primask = __get_PRIMASK();
            __disable_irq();

            if (state == DAMPER_STATE_POST_MOVE_HOLD)
            {
                damper_set_standby(false);
                damper_clear_outputs();
                phase_energized = false;
                released        = true;
                state           = DAMPER_STATE_IDLE_RELEASED;
            }

            if (primask == 0U)
            {
                __enable_irq();
            }
        }
    }
}

void AppDamper_TimerCallback(void)
{
#if APP_DAMPER_ENABLED
    if (!init_ok) return;
    if (!damper_is_moving()) return;
    if (remaining_steps == 0U) return;

    /* Boundary check BEFORE phase change */
    if (position_valid)
    {
        if (moving_forward)
        {
            if (current_steps >= (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
            {
                damper_enter_fault();
                return;
            }
        }
        else
        {
            if (current_steps <= 0)
            {
                damper_enter_fault();
                return;
            }
        }
    }

    /* Advance phase */
    if (moving_forward)
    {
        phase_index = (phase_index + 1U) & 0x03U;
    }
    else
    {
        phase_index = (phase_index + 3U) & 0x03U;
    }

    damper_write_phase((DamperPhaseIndex)(phase_index & 0x03U));

    /* Count this step */
    if (position_valid)
    {
        if (moving_forward)
        {
            current_steps++;
        }
        else
        {
            current_steps--;
        }
    }

    remaining_steps--;

    /* Final step completed? */
    if (remaining_steps == 0U)
    {
        __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
        __HAL_TIM_DISABLE(&htim6);
        __HAL_TIM_SET_COUNTER(&htim6, 0U);
        __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);

        state       = DAMPER_STATE_POST_MOVE_HOLD;
        last_result = DAMPER_RESULT_SUCCESS;
        hold_start_tick = HAL_GetTick();
    }
#endif
}

void AppDamper_EmergencyShutdown(void)
{
#if APP_DAMPER_ENABLED
    if (!gpio_initialized) return;

    if (tim6_initialized)
    {
        damper_tim6_reset();
    }

    damper_set_standby(false);
    damper_clear_outputs();

    state            = DAMPER_STATE_FAULT;
    released         = true;
    position_valid   = false;
    phase_energized  = false;
    remaining_steps  = 0U;
    last_result      = DAMPER_RESULT_ABORTED_BY_EMERGENCY;
    fault_flags     |= 0x01U;
#endif
}

bool AppDamper_GetSnapshot(DamperSnapshot *snap)
{
    if (snap == NULL) return false;

    snap->status        = 0x00U;
    snap->object_id     = 0x07U;

    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        snap->damper_state   = state;
        snap->current_steps  = current_steps;
        snap->target_steps   = target_steps;
        snap->remaining_steps = remaining_steps;
        snap->last_command   = (uint8_t)last_command;
        snap->last_result    = (uint8_t)last_result;
        snap->fault_flags    = fault_flags;

        {
            uint8_t f = 0x00U;
            if (position_valid)   f |= 0x01U;
            if (damper_is_moving())
            {
                f |= 0x02U;
                if (moving_forward) f |= 0x04U;
            }
            if (released)         f |= 0x08U;
#if APP_DAMPER_ENABLED
            if (position_valid)   f |= 0x10U;
#endif
            snap->flags = f;
        }

        if (primask == 0U)
        {
            __enable_irq();
        }
    }

    snap->full_travel_steps = APP_DAMPER_FULL_TRAVEL_STEPS;
    snap->configured_pps    = APP_DAMPER_STEP_PPS;

    return true;
}

uint8_t AppDamper_MoveAbsolute(int32_t target)
{
#if !APP_DAMPER_ENABLED
    (void)target;
    return DAMPER_STATUS_READ_ONLY;
#else
    if (!init_ok || !gpio_initialized || !tim6_initialized)
        return DAMPER_STATUS_HW_ERROR;
    if (state == DAMPER_STATE_FAULT)
        return DAMPER_STATUS_HW_ERROR;
    if (damper_is_busy())
        return DAMPER_STATUS_BUSY;
    if (!position_valid)
        return DAMPER_STATUS_NO_VALID_DATA;
    if (target < 0 || target > (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return DAMPER_STATUS_PARAM_RANGE;

    last_command = DAMPER_CMD_MOVE_ABSOLUTE;

    if (target == current_steps)
    {
        target_steps    = target;
        remaining_steps  = 0U;
        last_result     = DAMPER_RESULT_SUCCESS;
        return DAMPER_STATUS_OK;
    }

    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        if (target > current_steps)
        {
            target_steps = target;
            damper_start_motion((uint32_t)(target - current_steps), true);
        }
        else
        {
            target_steps = target;
            damper_start_motion((uint32_t)(current_steps - target), false);
        }

        if (primask == 0U)
        {
            __enable_irq();
        }
    }

    return DAMPER_STATUS_OK;
#endif
}

uint8_t AppDamper_MoveRelative(int32_t delta)
{
#if !APP_DAMPER_ENABLED
    (void)delta;
    return DAMPER_STATUS_READ_ONLY;
#else
    if (!init_ok || !gpio_initialized || !tim6_initialized)
        return DAMPER_STATUS_HW_ERROR;
    if (state == DAMPER_STATE_FAULT)
        return DAMPER_STATUS_HW_ERROR;
    if (damper_is_busy())
        return DAMPER_STATUS_BUSY;

    if (position_valid)
    {
        int64_t target64 = (int64_t)current_steps + (int64_t)delta;

        if (target64 < 0LL ||
            target64 > (int64_t)APP_DAMPER_FULL_TRAVEL_STEPS)
            return DAMPER_STATUS_PARAM_RANGE;

        int32_t new_target = (int32_t)target64;

        last_command = DAMPER_CMD_MOVE_RELATIVE;

        if (delta == 0)
        {
            target_steps    = new_target;
            remaining_steps  = 0U;
            last_result     = DAMPER_RESULT_SUCCESS;
            return DAMPER_STATUS_OK;
        }

        {
            uint32_t primask = __get_PRIMASK();
            __disable_irq();

            target_steps = new_target;

            if (delta > 0)
            {
                damper_start_motion((uint32_t)delta, true);
            }
            else
            {
                damper_start_motion((uint32_t)(-delta), false);
            }

            if (primask == 0U)
            {
                __enable_irq();
            }
        }

        return DAMPER_STATUS_OK;
    }
    else
    {
        if (delta < -(int32_t)APP_DAMPER_DIAG_MAX_RELATIVE_STEPS ||
            delta >  (int32_t)APP_DAMPER_DIAG_MAX_RELATIVE_STEPS)
            return DAMPER_STATUS_PARAM_RANGE;

        last_command = DAMPER_CMD_MOVE_RELATIVE;

        if (delta == 0)
        {
            remaining_steps = 0U;
            last_result     = DAMPER_RESULT_SUCCESS;
            return DAMPER_STATUS_OK;
        }

        {
            uint32_t steps;
            bool forward;
            uint32_t primask;

            if (delta > 0)
            {
                steps   = (uint32_t)delta;
                forward = true;
            }
            else
            {
                steps   = (uint32_t)(-delta);
                forward = false;
            }

            primask = __get_PRIMASK();
            __disable_irq();

            damper_start_motion(steps, forward);

            if (primask == 0U)
            {
                __enable_irq();
            }
        }

        return DAMPER_STATUS_OK;
    }
#endif
}

uint8_t AppDamper_SetCurrentPosition(int32_t position)
{
#if !APP_DAMPER_ENABLED
    (void)position;
    return DAMPER_STATUS_READ_ONLY;
#else
    if (!init_ok || !gpio_initialized)
        return DAMPER_STATUS_HW_ERROR;
    if (state == DAMPER_STATE_FAULT)
        return DAMPER_STATUS_HW_ERROR;
    if (damper_is_busy())
        return DAMPER_STATUS_BUSY;
    if (position < 0 || position > (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return DAMPER_STATUS_PARAM_RANGE;

    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        if (gpio_initialized)
        {
            damper_set_standby(false);
            damper_clear_outputs();
        }

        current_steps   = position;
        target_steps    = position;
        remaining_steps  = 0U;
        position_valid   = true;
        phase_energized  = false;
        released         = true;
        state            = DAMPER_STATE_IDLE_RELEASED;

        if (primask == 0U)
        {
            __enable_irq();
        }
    }

    return DAMPER_STATUS_OK;
#endif
}

uint8_t AppDamper_Stop(void)
{
#if !APP_DAMPER_ENABLED
    return DAMPER_STATUS_READ_ONLY;
#else
    if (!init_ok || !gpio_initialized)
        return DAMPER_STATUS_HW_ERROR;

    if (damper_is_moving())
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        damper_tim6_reset();

        if (gpio_initialized)
        {
            damper_set_standby(false);
            damper_clear_outputs();
        }

        phase_energized  = false;
        remaining_steps  = 0U;
        released         = true;
        target_steps     = current_steps;
        state            = DAMPER_STATE_STOPPED;
        last_result      = DAMPER_RESULT_ABORTED_BY_STOP;

        if (primask == 0U)
        {
            __enable_irq();
        }

        return DAMPER_STATUS_OK;
    }

    if (state == DAMPER_STATE_POST_MOVE_HOLD)
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        if (state == DAMPER_STATE_POST_MOVE_HOLD)
        {
            damper_tim6_reset();
            damper_set_standby(false);
            damper_clear_outputs();
            phase_energized = false;
            released        = true;
            state           = DAMPER_STATE_STOPPED;
        }

        if (primask == 0U)
        {
            __enable_irq();
        }

        return DAMPER_STATUS_OK;
    }

    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        damper_tim6_reset();

        if (gpio_initialized)
        {
            damper_set_standby(false);
            damper_clear_outputs();
        }

        phase_energized = false;
        remaining_steps  = 0U;
        released        = true;
        state           = DAMPER_STATE_STOPPED;

        if (primask == 0U)
        {
            __enable_irq();
        }
    }

    return DAMPER_STATUS_OK;
#endif
}

uint8_t AppDamper_Release(void)
{
#if !APP_DAMPER_ENABLED
    return DAMPER_STATUS_READ_ONLY;
#else
    if (!init_ok || !gpio_initialized)
        return DAMPER_STATUS_HW_ERROR;

    if (damper_is_moving())
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        damper_tim6_reset();

        if (gpio_initialized)
        {
            damper_set_standby(false);
            damper_clear_outputs();
        }

        phase_energized  = false;
        remaining_steps  = 0U;
        released         = true;
        target_steps     = current_steps;
        state            = DAMPER_STATE_IDLE_RELEASED;
        last_result      = DAMPER_RESULT_ABORTED_BY_RELEASE;

        if (primask == 0U)
        {
            __enable_irq();
        }

        return DAMPER_STATUS_OK;
    }

    if (state == DAMPER_STATE_POST_MOVE_HOLD)
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        if (state == DAMPER_STATE_POST_MOVE_HOLD)
        {
            damper_tim6_reset();
            damper_set_standby(false);
            damper_clear_outputs();
            phase_energized = false;
            released        = true;
            state           = DAMPER_STATE_IDLE_RELEASED;
        }

        if (primask == 0U)
        {
            __enable_irq();
        }

        return DAMPER_STATUS_OK;
    }

    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        damper_tim6_reset();

        if (gpio_initialized)
        {
            damper_set_standby(false);
            damper_clear_outputs();
        }

        phase_energized = false;
        remaining_steps  = 0U;
        released        = true;
        state           = DAMPER_STATE_IDLE_RELEASED;

        if (primask == 0U)
        {
            __enable_irq();
        }
    }

    return DAMPER_STATUS_OK;
#endif
}
