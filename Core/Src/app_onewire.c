/**
 * @file app_onewire.c
 * @brief 单总线角色无关门面层（实现文件）。
 *
 * 模块职责：根据编译期角色把统一 API 转发到 Master 或 Slave 实现，使 W2、LED 和 main.c 不需要散布角色判断。
 * 数据输入：编译宏 APP_ONEWIRE_ROLE；上层提交和查询。
 * 数据输出：统一角色、链路、提交和快照 API。
 * 执行上下文：Master 构建包含多从机事务；Slave 构建只运行本机从机状态机。
 * 阅读重点：这是理解编译期角色隔离的入口，先读此文件再分别进入 master/slave。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_onewire.h"

#include <stddef.h>

#include "app_onewire_config.h"
#include "app_onewire_master.h"
#include "app_onewire_slave.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_SLAVE)
/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppOneWireLinkState slave_link_state(void)
{
    AppOneWireSlaveState state = AppOneWireSlave_GetState();

    if (state == APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1)
    {
        return APP_ONEWIRE_LINK_OFFLINE;
    }
    if (state == APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2)
    {
        return APP_ONEWIRE_LINK_HANDSHAKING;
    }
    if (state == APP_ONEWIRE_SLAVE_ONLINE)
    {
        return APP_ONEWIRE_LINK_ONLINE;
    }

    return APP_ONEWIRE_LINK_STALE;
}
#endif

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppOneWire_Init(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    AppOneWireMaster_Init();
#else
    AppOneWireSlave_Init();
#endif
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppOneWire_Process(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    AppOneWireMaster_Process();
#else
    AppOneWireSlave_Process();
#endif
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireRole AppOneWire_GetRole(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return APP_ONEWIRE_ROLE_VALUE_MASTER;
#else
    return APP_ONEWIRE_ROLE_VALUE_SLAVE;
#endif
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
uint8_t AppOneWire_GetLocalSlaveAddress(void)
{
    return APP_ONEWIRE_SLAVE_ADDRESS;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireLinkState AppOneWire_GetLinkState(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return AppOneWireMaster_GetLinkState();
#else
    return slave_link_state();
#endif
}

/**
 * @brief 向默认目标提交异步事务。
 * @param operation 协议操作码。
 * @param address 寄存器地址。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireSubmitResult AppOneWire_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    return AppOneWire_SubmitTo(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS,
        operation,
        address,
        value);
}

/**
 * @brief 向指定目标提交异步事务。
 * @param slave_address 目标从机地址。
 * @param operation 协议操作码。
 * @param address 寄存器地址。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireSubmitResult AppOneWire_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return AppOneWireMaster_SubmitTo(
        slave_address,
        operation,
        address,
        value);
#else
    (void)slave_address;
    (void)operation;
    (void)address;
    (void)value;
    return APP_ONEWIRE_SUBMIT_NOT_MASTER;
#endif
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param slave_address 目标从机地址。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppOneWire_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    {
        AppOneWireMasterSnapshot master_snapshot;
        bool context_valid = AppOneWireMaster_GetSnapshotForAddress(
            slave_address,
            &master_snapshot);

        snapshot->role = APP_ONEWIRE_ROLE_VALUE_MASTER;
        snapshot->link_state = master_snapshot.link_state;
        snapshot->busy = master_snapshot.busy;
        snapshot->pending_valid = master_snapshot.pending_valid;
        snapshot->last_operation = master_snapshot.last_operation;
        snapshot->result_code = master_snapshot.result_code;
        snapshot->address = master_snapshot.address;
        snapshot->value = master_snapshot.value;
        snapshot->slave_address = master_snapshot.slave_address;
        snapshot->context_valid = master_snapshot.context_valid;
        snapshot->last_response_age_ms =
            master_snapshot.last_response_age_ms;
        return context_valid;
    }
#else
    snapshot->role = APP_ONEWIRE_ROLE_VALUE_SLAVE;
    snapshot->slave_address = slave_address;
    snapshot->context_valid =
        (slave_address == APP_ONEWIRE_SLAVE_ADDRESS);
    snapshot->link_state = snapshot->context_valid
        ? slave_link_state()
        : APP_ONEWIRE_LINK_OFFLINE;
    snapshot->busy = snapshot->context_valid &&
                     AppOneWireSlave_IsResponsePending();
    snapshot->pending_valid = snapshot->busy;
    snapshot->last_operation = 0U;
    snapshot->result_code = APP_ONEWIRE_RESULT_SUCCESS;
    snapshot->address = 0U;
    snapshot->value = 0U;
    snapshot->last_response_age_ms = 0xFFFFU;
    return snapshot->context_valid;
#endif
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 */
void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    (void)AppOneWire_GetSnapshotForAddress(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS,
        snapshot);
#else
    (void)AppOneWire_GetSnapshotForAddress(
        APP_ONEWIRE_SLAVE_ADDRESS,
        snapshot);
#endif
}
