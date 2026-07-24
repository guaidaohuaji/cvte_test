#ifndef APP_ONEWIRE_SLAVE_H
#define APP_ONEWIRE_SLAVE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1 = 0,
    APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2,
    APP_ONEWIRE_SLAVE_ONLINE,
    APP_ONEWIRE_SLAVE_COMM_FAULT
} AppOneWireSlaveState;

typedef struct
{
    uint32_t parser_frame_count;
    uint32_t parser_xor_error_count;
    uint32_t parser_format_error_count;
    uint32_t parser_timeout_count;
    uint32_t handshake_1_count;
    uint32_t handshake_2_count;
    uint32_t handshake_timeout_count;
    uint32_t write_count;
    uint32_t read_count;
    uint32_t invalid_frame_count;
    uint32_t invalid_address_count;
    uint32_t invalid_operation_count;
    uint32_t response_queued_count;
    uint32_t response_sent_count;
    uint32_t response_send_fail_count;
    uint32_t response_busy_drop_count;
    uint32_t comm_fault_count;
} AppOneWireSlaveStats;

void AppOneWireSlave_Init(void);
void AppOneWireSlave_Process(void);

AppOneWireSlaveState AppOneWireSlave_GetState(void);
bool AppOneWireSlave_IsResponsePending(void);
bool AppOneWireSlave_ReadRegister(uint16_t address, uint16_t *value);
void AppOneWireSlave_GetStats(AppOneWireSlaveStats *stats);

#endif
