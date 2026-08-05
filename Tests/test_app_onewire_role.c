#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_onewire.h"
#include "app_onewire_config.h"
#include "app_onewire_master.h"
#include "app_onewire_slave.h"

static unsigned master_init_count;
static unsigned master_process_count;
static unsigned slave_init_count;
static unsigned slave_process_count;
static AppOneWireSubmitResult fake_submit_result;
static AppOneWireMasterSnapshot fake_master_snapshot;
static AppOneWireSlaveState fake_slave_state;
static bool fake_slave_response_pending;

void AppOneWireMaster_Init(void)
{
    master_init_count++;
}

void AppOneWireMaster_Process(void)
{
    master_process_count++;
}

AppOneWireSubmitResult AppOneWireMaster_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    return AppOneWireMaster_SubmitTo(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS, operation, address, value);
}

AppOneWireSubmitResult AppOneWireMaster_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    (void)slave_address;
    (void)operation;
    (void)address;
    (void)value;
    return fake_submit_result;
}

void AppOneWireMaster_GetSnapshot(AppOneWireMasterSnapshot *snapshot)
{
    *snapshot = fake_master_snapshot;
}

bool AppOneWireMaster_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireMasterSnapshot *snapshot)
{
    *snapshot = fake_master_snapshot;
    snapshot->slave_address = slave_address;
    snapshot->context_valid = true;
    return true;
}

AppOneWireLinkState AppOneWireMaster_GetLinkState(void)
{
    return fake_master_snapshot.link_state;
}

void AppOneWireSlave_Init(void)
{
    slave_init_count++;
}

void AppOneWireSlave_Process(void)
{
    slave_process_count++;
}

AppOneWireSlaveState AppOneWireSlave_GetState(void)
{
    return fake_slave_state;
}

bool AppOneWireSlave_IsResponsePending(void)
{
    return fake_slave_response_pending;
}

static void reset_fake(void)
{
    master_init_count = 0U;
    master_process_count = 0U;
    slave_init_count = 0U;
    slave_process_count = 0U;
    fake_submit_result = APP_ONEWIRE_SUBMIT_OK;
    (void)memset(&fake_master_snapshot, 0, sizeof(fake_master_snapshot));
    fake_slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1;
    fake_slave_response_pending = false;
}

int main(void)
{
    AppOneWireSnapshot snapshot;

    reset_fake();
    AppOneWire_Init();
    AppOneWire_Process();

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
    assert(AppOneWire_GetRole() == APP_ONEWIRE_ROLE_VALUE_MASTER);
    assert(master_init_count == 1U);
    assert(master_process_count == 1U);
    assert(slave_init_count == 0U);
    assert(slave_process_count == 0U);

    fake_master_snapshot.link_state = APP_ONEWIRE_LINK_ONLINE;
    fake_master_snapshot.busy = true;
    fake_master_snapshot.pending_valid = true;
    fake_master_snapshot.last_operation = 0x03U;
    fake_master_snapshot.result_code = APP_ONEWIRE_RESULT_PENDING;
    fake_master_snapshot.address = 0x0008U;
    fake_master_snapshot.value = 0x1234U;
    fake_master_snapshot.slave_address = APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS;
    fake_master_snapshot.context_valid = true;
    fake_master_snapshot.last_response_age_ms = 12U;

    assert(AppOneWire_Submit(0x03U, 0x0008U, 0x1234U) ==
           APP_ONEWIRE_SUBMIT_OK);
    AppOneWire_GetSnapshot(&snapshot);
    assert(snapshot.role == APP_ONEWIRE_ROLE_VALUE_MASTER);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_ONLINE);
    assert(snapshot.busy);
    assert(snapshot.pending_valid);
    assert(snapshot.last_operation == 0x03U);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_PENDING);
    assert(snapshot.address == 0x0008U);
    assert(snapshot.value == 0x1234U);
    assert(snapshot.slave_address == APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS);
    assert(snapshot.context_valid);
    assert(snapshot.last_response_age_ms == 12U);
#else
    assert(AppOneWire_GetRole() == APP_ONEWIRE_ROLE_VALUE_SLAVE);
    assert(master_init_count == 0U);
    assert(master_process_count == 0U);
    assert(slave_init_count == 1U);
    assert(slave_process_count == 1U);
    assert(AppOneWire_Submit(0x03U, 0x0008U, 0x1234U) ==
           APP_ONEWIRE_SUBMIT_NOT_MASTER);
    assert(AppOneWire_SubmitTo(0x03U, 0x03U, 0x0008U, 0x1234U) ==
           APP_ONEWIRE_SUBMIT_NOT_MASTER);

    fake_slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2;
    fake_slave_response_pending = true;
    AppOneWire_GetSnapshot(&snapshot);
    assert(snapshot.role == APP_ONEWIRE_ROLE_VALUE_SLAVE);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_HANDSHAKING);
    assert(snapshot.busy);
    assert(snapshot.pending_valid);

    fake_slave_state = APP_ONEWIRE_SLAVE_ONLINE;
    assert(AppOneWire_GetLinkState() == APP_ONEWIRE_LINK_ONLINE);

    fake_slave_state = APP_ONEWIRE_SLAVE_COMM_FAULT;
    assert(AppOneWire_GetLinkState() == APP_ONEWIRE_LINK_STALE);
#endif

    puts("app_onewire role dispatch tests passed");
    return 0;
}
