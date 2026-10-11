#include <stddef.h>
#include <stdint.h>

#include "modbus_rtu.h"
#include "modbus_mock.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void finish(uint8_t *response, size_t body,
                    size_t *response_length)
{
    uint16_t crc = amp_mb_crc16(response, body);

    response[body] = (uint8_t)crc;
    response[body + 1U] = (uint8_t)(crc >> 8);
    *response_length = body + 2U;
}

static void exception_response(uint8_t slave, uint8_t function,
                                uint8_t exception, uint8_t *response,
                                size_t *response_length)
{
    response[0] = slave;
    response[1] = (uint8_t)(function | 0x80U);
    response[2] = exception;
    finish(response, 3U, response_length);
}

int amp_mb_mock_init(AmpModbusMock *mock, uint8_t slave)
{
    if (mock == NULL || slave == 0U || slave > 247U)
        return -1;

    mock->slave = slave;
    mock->registers[0] = 256U;
    mock->registers[1] = 603U;
    mock->registers[2] = 1U;
    mock->registers[3] = 0U;

    return 0;
}

int amp_mb_mock_reply(AmpModbusMock *mock,
                       const uint8_t *request,
                       size_t request_length,
                       uint8_t *response,
                       size_t capacity,
                       size_t *response_length)
{
    uint16_t crc, address, value;
    size_t i;

    if (response_length == NULL)
        return -1;
    *response_length = 0U;

    if (mock == NULL || request == NULL || response == NULL ||
        capacity < 13U || mock->slave == 0U || mock->slave > 247U)
        return -1;

    if (request_length != 8U || request[0] != mock->slave)
        return 1;

    crc = (uint16_t)request[6] |
          (uint16_t)((uint16_t)request[7] << 8);

    if (amp_mb_crc16(request, 6U) != crc)
        return 1;

    address = be16(request + 2);
    value = be16(request + 4);

    if (request[1] == 3U) {
        if (value == 0U || value > 125U) {
            exception_response(mock->slave, 3U, 3U,
                               response, response_length);
            return 0;
        }

        if ((uint32_t)address + value > 4U) {
            exception_response(mock->slave, 3U, 2U,
                               response, response_length);
            return 0;
        }

        response[0] = mock->slave;
        response[1] = 3U;
        response[2] = (uint8_t)(value * 2U);

        for (i = 0; i < value; ++i) {
            uint16_t reg = mock->registers[address + i];
            response[3U + i * 2U] = (uint8_t)(reg >> 8);
            response[4U + i * 2U] = (uint8_t)reg;
        }

        finish(response, 3U + (size_t)value * 2U, response_length);
        return 0;
    }

    if (request[1] == 6U) {
        if (address >= 4U) {
            exception_response(mock->slave, 6U, 2U,
                               response, response_length);
            return 0;
        }

        mock->registers[address] = value;

        for (i = 0; i < 6U; ++i)
            response[i] = request[i];

        finish(response, 6U, response_length);
        return 0;
    }

    exception_response(mock->slave, request[1], 1U,
                       response, response_length);
    return 0;
}
