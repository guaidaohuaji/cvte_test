/**
 * @file app_onewire_config.h
 * @brief 单总线角色、地址和超时配置。
 *
 * 配置职责：定义 Master/Slave 角色、AA 帧限制、握手/响应/500 ms 失效时序、地址范围、上下文数量与寄存器范围。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_ONEWIRE_CONFIG_H
#define APP_ONEWIRE_CONFIG_H

#include <stdint.h>

#define APP_ONEWIRE_ROLE_MASTER                 1U
#define APP_ONEWIRE_ROLE_SLAVE                  2U

#ifndef APP_ONEWIRE_ROLE
#error "APP_ONEWIRE_ROLE must be defined as APP_ONEWIRE_ROLE_MASTER or APP_ONEWIRE_ROLE_SLAVE"
#endif

#if ((APP_ONEWIRE_ROLE != APP_ONEWIRE_ROLE_MASTER) && \
     (APP_ONEWIRE_ROLE != APP_ONEWIRE_ROLE_SLAVE))
#error "APP_ONEWIRE_ROLE has an invalid value"
#endif

#define APP_ONEWIRE_FRAME_HEADER               0xAAU
#define APP_ONEWIRE_MAX_DATA_LEN               32U
#define APP_ONEWIRE_FRAME_OVERHEAD             5U
#define APP_ONEWIRE_MAX_FRAME_LEN              \
    (APP_ONEWIRE_MAX_DATA_LEN + APP_ONEWIRE_FRAME_OVERHEAD)

#define APP_ONEWIRE_INTERBYTE_TIMEOUT_MS       50U

#define APP_ONEWIRE_HANDSHAKE_TOTAL_MS          500U
#define APP_ONEWIRE_HANDSHAKE_RESPONSE_MS       100U
#define APP_ONEWIRE_OPERATION_RESPONSE_MS       250U
#define APP_ONEWIRE_TX_COMPLETE_TIMEOUT_MS      100U
#define APP_ONEWIRE_MASTER_LINK_VALID_MS        450U
#define APP_ONEWIRE_MASTER_GUARD_MS               3U
#define APP_ONEWIRE_SLAVE_FAULT_MS              500U
#define APP_ONEWIRE_RESPONSE_TURNAROUND_MS        2U

#define APP_ONEWIRE_OPERATION_REHANDSHAKE        0x01U
#define APP_ONEWIRE_OPERATION_WRITE              0x03U
#define APP_ONEWIRE_OPERATION_READ               0x06U

#define APP_ONEWIRE_UART_RX_RING_SIZE           256U

#define APP_ONEWIRE_LOCAL_ADDR_MASTER           0x01U
#define APP_ONEWIRE_SLAVE_ADDR_MIN              0x02U
#define APP_ONEWIRE_SLAVE_ADDR_MAX              0xFEU
#define APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS       0x02U
#define APP_ONEWIRE_MASTER_MAX_SLAVES             8U

#ifndef APP_ONEWIRE_SLAVE_ADDRESS
#define APP_ONEWIRE_SLAVE_ADDRESS APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS
#endif

#if ((APP_ONEWIRE_SLAVE_ADDRESS < APP_ONEWIRE_SLAVE_ADDR_MIN) || \
     (APP_ONEWIRE_SLAVE_ADDRESS > APP_ONEWIRE_SLAVE_ADDR_MAX))
#error "APP_ONEWIRE_SLAVE_ADDRESS must be in the range 0x02..0xFE"
#endif

/* Compatibility alias for older tests and local slave-only code.  Master
 * routing must use an explicit transaction target instead of this alias. */
#define APP_ONEWIRE_LOCAL_ADDR_SLAVE APP_ONEWIRE_SLAVE_ADDRESS

#define APP_ONEWIRE_REGISTER_MAX_ADDR          0x0100U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
