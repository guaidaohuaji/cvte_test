#ifndef APP_ONEWIRE_MASTER_H
#define APP_ONEWIRE_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_onewire.h"

typedef enum
{
    APP_ONEWIRE_MASTER_BOOT = 0,
    APP_ONEWIRE_MASTER_HANDSHAKE_START,
    APP_ONEWIRE_MASTER_HANDSHAKE_1_TX,
    APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT,
    APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2,
    APP_ONEWIRE_MASTER_HANDSHAKE_2_TX,
    APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT,
    APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION,
    APP_ONEWIRE_MASTER_ONLINE_IDLE,
    APP_ONEWIRE_MASTER_WRITE_TX,
    APP_ONEWIRE_MASTER_WRITE_WAIT,
    APP_ONEWIRE_MASTER_READ_TX,
    APP_ONEWIRE_MASTER_READ_WAIT,
    APP_ONEWIRE_MASTER_STALE_IDLE,
    APP_ONEWIRE_MASTER_ERROR_IDLE
} AppOneWireMasterState;

typedef struct
{
    AppOneWireLinkState link_state;
    AppOneWireMasterState master_state;
    bool busy;
    bool pending_valid;
    uint8_t last_operation;
    AppOneWireResultCode result_code;
    uint16_t address;
    uint16_t value;
} AppOneWireMasterSnapshot;

typedef struct
{
    uint32_t parser_frame_count;
    uint32_t parser_xor_error_count;
    uint32_t parser_format_error_count;
    uint32_t parser_timeout_count;
    uint32_t unexpected_frame_count;
    uint32_t response_error_count;
    uint32_t handshake_attempt_count;
    uint32_t handshake_1_ok_count;
    uint32_t handshake_2_ok_count;
    uint32_t handshake_timeout_count;
    uint32_t write_tx_count;
    uint32_t write_ok_count;
    uint32_t read_tx_count;
    uint32_t read_ok_count;
    uint32_t operation_timeout_count;
    uint32_t tx_timeout_count;
    uint32_t uart_error_count;
} AppOneWireMasterStats;

void AppOneWireMaster_Init(void);
void AppOneWireMaster_Process(void);

AppOneWireSubmitResult AppOneWireMaster_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value);

void AppOneWireMaster_GetSnapshot(AppOneWireMasterSnapshot *snapshot);
void AppOneWireMaster_GetStats(AppOneWireMasterStats *stats);

AppOneWireMasterState AppOneWireMaster_GetState(void);
AppOneWireLinkState AppOneWireMaster_GetLinkState(void);
bool AppOneWireMaster_IsBusy(void);

#endif
