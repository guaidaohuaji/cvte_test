/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_uart.h"
#include "app_pwm.h"
#include "app_pwm_input.h"
#include "app_ntc.h"
#include "app_fan.h"
#include "app_adc_scan.h"
#include "app_fan_feedback_adc.h"
#include "app_fan_feedback_bpf.h"
#include "app_onewire_uart.h"
#include "app_onewire.h"
#include "app_led.h"
#include "app_damper.h"
#include "app_auto_control.h"
#include "app_manual_fan_control.h"
#include "app_fan_health.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/*
 * 工程运行模型
 * ------------
 * 这是一个“中断搬运 + 主循环状态机”的裸机工程：
 * - UART/ADC/TIM 中断只接收字节、置标志或推进一个步进相位；
 * - 业务计算、协议解析、故障判断均在 while(1) 中完成；
 * - 每个 AppXXX_Process() 都必须快速返回，禁止 HAL_Delay()。
 *
 * 建议阅读者把 main.c 当作全工程目录：初始化顺序说明资源依赖，主循环
 * 顺序说明同一轮中哪些模块先看到新数据、哪些模块会晚一轮看到。
 */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART6_UART_Init();
  MX_TIM4_Init();
  MX_TIM1_Init();
  MX_ADC1_Init();
  MX_TIM10_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
#if APP_DAMPER_ENABLED
  MX_TIM6_Init();
#endif
  /* 1) 先启动两个通信入口。USART1 面向上位机，USART6 面向单总线。 */
  AppUart_Init();
  AppOneWireUart_Init();
  AppOneWire_Init();
  /* 2) LED 依赖单总线角色/状态，因此在 AppOneWire_Init() 之后初始化。 */
  AppLed_Init();
  /* 3) 通用 PWM 输出/输入属于独立可查询对象；初始化失败视为致命错误。 */
  if (!AppPwm_Init())
  {
    Error_Handler();
  }
  if (!AppPwmInput_Init())
  {
    Error_Handler();
  }
  /* 4) 传感器与风机链：先建立业务状态，再启动共享 ADC DMA 数据源。 */
  (void)AppNtc_Init();
  (void)AppFan_Init();
  (void)AppAdcScan_Init();
#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
  AppFanFeedbackBpf_Init();
#endif
  /* 5) 执行机构与控制器：Master 构建中风门会立即开始上电全开校准。 */
  AppDamper_Init();
  AppAutoControl_Init();
  AppManualFanControl_Init();
  AppFanHealth_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 通信底层先处理，确保本轮尽早释放 TX/RX 状态和读取新字节。 */
    AppOneWireUart_Process();
    AppOneWire_Process();
    /* W2 命令可能在本轮改变控制目标，故放在各业务状态机之前。 */
    AppUart_Process();
    AppLed_Process();
    /* 独立测量与传感器任务：均为轮询式、无阻塞。 */
    AppPwmInput_Process();
    AppNtc_Process();
    AppFan_Process();
    /* ADC DMA 块先整理，再由风机反馈模块消费已发布的最新 CH0 块。 */
    AppAdcScan_Process();
    AppFanFeedback_Process();
    /* 执行机构先更新实际状态，AUTO/手动控制随后依据最新快照下发目标。 */
    AppDamper_Process();
    AppAutoControl_Process();
    AppManualFanControl_Process();
    /* 健康诊断最后观察本轮最终 applied PWM 与 RPM，并可立即锁存停机。 */
    AppFanHealth_Process();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/*
 * HAL 回调集中转发原则：同一种 HAL weak callback 在全工程只能有一个
 * 强定义，因此在 main.c 中按外设实例分发到各应用模块。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    AppUart_RxCpltCallback();
  }
  else if (huart->Instance == USART6)
  {
    AppOneWireUart_RxCpltCallback();
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART6)
  {
    AppOneWireUart_TxCpltCallback();
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    AppUart_ErrorCallback(huart);
  }
  else if (huart->Instance == USART6)
  {
    AppOneWireUart_ErrorCallback(huart->ErrorCode);
  }
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
  AppPwmInput_CC_Callback(htim);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1)
  {
    AppPwmInput_UP_Callback(htim);
  }
  else if (htim->Instance == TIM6)
  {
    AppDamper_TimerCallback();
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  AppDamper_EmergencyShutdown();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
