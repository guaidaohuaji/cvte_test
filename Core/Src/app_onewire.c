#include "app_onewire.h"

#include <stddef.h>

#include "app_onewire_config.h"
#include "app_onewire_master.h"
#include "app_onewire_slave.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_SLAVE)
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

void AppOneWire_Init(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    AppOneWireMaster_Init();
#else
    AppOneWireSlave_Init();
#endif
}

void AppOneWire_Process(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    AppOneWireMaster_Process();
#else
    AppOneWireSlave_Process();
#endif
}

AppOneWireRole AppOneWire_GetRole(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return APP_ONEWIRE_ROLE_VALUE_MASTER;
#else
    return APP_ONEWIRE_ROLE_VALUE_SLAVE;
#endif
}

AppOneWireLinkState AppOneWire_GetLinkState(void)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return AppOneWireMaster_GetLinkState();
#else
    return slave_link_state();
#endif
}

AppOneWireSubmitResult AppOneWire_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    return AppOneWireMaster_Submit(operation, address, value);
#else
    (void)operation;
    (void)address;
    (void)value;
    return APP_ONEWIRE_SUBMIT_NOT_MASTER;
#endif
}

void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    {
        AppOneWireMasterSnapshot master_snapshot;

        AppOneWireMaster_GetSnapshot(&master_snapshot);
        snapshot->role = APP_ONEWIRE_ROLE_VALUE_MASTER;
        snapshot->link_state = master_snapshot.link_state;
        snapshot->busy = master_snapshot.busy;
        snapshot->pending_valid = master_snapshot.pending_valid;
        snapshot->last_operation = master_snapshot.last_operation;
        snapshot->result_code = master_snapshot.result_code;
        snapshot->address = master_snapshot.address;
        snapshot->value = master_snapshot.value;
    }
#else
    snapshot->role = APP_ONEWIRE_ROLE_VALUE_SLAVE;
    snapshot->link_state = slave_link_state();
    snapshot->busy = AppOneWireSlave_IsResponsePending();
    snapshot->pending_valid = snapshot->busy;
    snapshot->last_operation = 0U;
    snapshot->result_code = APP_ONEWIRE_RESULT_SUCCESS;
    snapshot->address = 0U;
    snapshot->value = 0U;
#endif
}
