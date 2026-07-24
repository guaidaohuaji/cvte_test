#ifndef APP_ONEWIRE_H
#define APP_ONEWIRE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_ONEWIRE_ROLE_VALUE_MASTER = 1,
    APP_ONEWIRE_ROLE_VALUE_SLAVE = 2
} AppOneWireRole;

typedef enum
{
    APP_ONEWIRE_LINK_OFFLINE = 0,
    APP_ONEWIRE_LINK_HANDSHAKING = 1,
    APP_ONEWIRE_LINK_ONLINE = 2,
    APP_ONEWIRE_LINK_STALE = 3,
    APP_ONEWIRE_LINK_UART_ERROR = 4
} AppOneWireLinkState;

typedef enum
{
    APP_ONEWIRE_RESULT_SUCCESS = 0x00,
    APP_ONEWIRE_RESULT_PENDING = 0x01,
    APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT = 0x08,
    APP_ONEWIRE_RESULT_RESPONSE_ERROR = 0x09,
    APP_ONEWIRE_RESULT_HANDSHAKE_FAILED = 0x0A,
    APP_ONEWIRE_RESULT_UART_ERROR = 0x0B
} AppOneWireResultCode;

typedef enum
{
    APP_ONEWIRE_SUBMIT_OK = 0,
    APP_ONEWIRE_SUBMIT_BUSY,
    APP_ONEWIRE_SUBMIT_INVALID_OPERATION,
    APP_ONEWIRE_SUBMIT_INVALID_ADDRESS,
    APP_ONEWIRE_SUBMIT_NOT_MASTER
} AppOneWireSubmitResult;

typedef struct
{
    AppOneWireRole role;
    AppOneWireLinkState link_state;
    bool busy;
    bool pending_valid;
    uint8_t last_operation;
    AppOneWireResultCode result_code;
    uint16_t address;
    uint16_t value;
} AppOneWireSnapshot;

void AppOneWire_Init(void);
void AppOneWire_Process(void);

AppOneWireRole AppOneWire_GetRole(void);
AppOneWireLinkState AppOneWire_GetLinkState(void);

AppOneWireSubmitResult AppOneWire_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value);

void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot);

#endif
