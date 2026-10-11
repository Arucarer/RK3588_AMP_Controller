#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amp_ipc.h"
#include "../rtos/protocol/modbus_rtu.h"

#define BUS_OPCODE 0x30U

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int number(const char *s, uint32_t max, uint32_t *out)
{
    char *end;
    unsigned long value;

    if (!s || !*s || *s == '-' || *s == '+' ||
        *s == ' ' || *s == '\t')
        return -1;

    errno = 0;
    value = strtoul(s, &end, 0);
    if (errno || *end || value > max)
        return -1;

    *out = (uint32_t)value;
    return 0;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int hexdata(const char *s, uint8_t *out, uint32_t *length)
{
    size_t n = strlen(s), i;

    if (n > 16U || n % 2U)
        return -1;

    for (i = 0; i < n / 2U; ++i) {
        int high = nibble(s[i * 2U]);
        int low = nibble(s[i * 2U + 1U]);
        if (high < 0 || low < 0)
            return -1;
        out[i] = (uint8_t)((high << 4) | low);
    }

    *length = (uint32_t)(n / 2U);
    return 0;
}

static const char *status_name(uint32_t status)
{
    switch (status) {
    case 0U: return "OK";
    case UINT32_MAX: return "ARGUMENT";
    case UINT32_MAX - 1U: return "NOT_READY";
    case UINT32_MAX - 2U: return "TIMEOUT_OR_MOCK_NO_RESPONSE";
    case UINT32_MAX - 3U: return "HW_ERROR";
    case UINT32_MAX - 4U: return "UNSUPPORTED";
    default: return "UNKNOWN";
    }
}

static int usage(const char *name)
{
    fprintf(stderr,
        "usage:\n"
        "  %s can-loop ID HEXDATA\n"
        "  %s uart-loop BYTE\n"
        "  %s mock-read ADDRESS COUNT\n"
        "  %s mock-write ADDRESS VALUE\n",
        name, name, name, name);
    return 2;
}

int main(int argc, char **argv)
{
    AMPClient client;
    AMPMessage response;
    uint8_t payload[20] = {0};
    uint8_t request[8] = {0};
    uint32_t operation, id = 0U, length = 0U;
    uint32_t a, b, status, returned;
    unsigned int i;
    int is_mock = 0;

    if (argc < 2)
        return usage(argv[0]);

    if (!strcmp(argv[1], "can-loop") && argc == 4) {
        operation = 1U;
        if (number(argv[2], 0x7FFU, &id) ||
            hexdata(argv[3], payload + 12, &length))
            return usage(argv[0]);
    } else if (!strcmp(argv[1], "uart-loop") && argc == 3) {
        operation = 2U;
        if (number(argv[2], 255U, &a))
            return usage(argv[0]);
        payload[12] = (uint8_t)a;
        length = 1U;
    } else if (argc == 4 &&
               (!strcmp(argv[1], "mock-read") ||
                !strcmp(argv[1], "mock-write"))) {
        uint8_t function =
            !strcmp(argv[1], "mock-read") ? 3U : 6U;

        operation = 3U;
        is_mock = 1;
        if (number(argv[2], 65535U, &a) ||
            number(argv[3], 65535U, &b))
            return usage(argv[0]);

        if (amp_mb_build_request(1U, function,
                                (uint16_t)a, (uint16_t)b,
                                request) != AMP_MB_OK)
            return usage(argv[0]);

        memcpy(payload + 12, request, 8U);
        length = 8U;
    } else {
        return usage(argv[0]);
    }

    put32(payload, operation);
    put32(payload + 4, id);
    put32(payload + 8, length);

    if (amp_client_open(&client)) {
        perror("amp_client_open");
        return 1;
    }

    if (amp_client_call(&client, BUS_OPCODE,
                        payload, sizeof(payload), &response)) {
        perror("amp_client_call");
        amp_client_close(&client);
        return 1;
    }
    amp_client_close(&client);

    if (response.opcode != BUS_OPCODE ||
        response.status != AMP_RESULT_OK ||
        response.payload_length != 36U ||
        get32(response.payload) != operation) {
        fprintf(stderr, "Invalid/rejected bus response: status=%u\n",
                response.status);
        return 1;
    }

    status = get32(response.payload + 4);
    returned = get32(response.payload + 8);

    if (returned > 16U) {
        fprintf(stderr, "Invalid returned length\n");
        return 1;
    }

    printf("MODE=%s RESULT=%s CLOCK=%u SAMPLE=%u DATA=",
           is_mock ? "MOCK" : "HARDWARE_LOOPBACK",
           status_name(status),
           get32(response.payload + 12),
           get32(response.payload + 16));

    for (i = 0; i < returned; ++i)
        printf("%02x", response.payload[20U + i]);
    puts("");

    if (status != 0U)
        return 1;

    if (is_mock) {
        uint16_t registers[125];
        uint8_t exception = 0U;
        int ret = amp_mb_parse_response(
            request, response.payload + 20, returned,
            registers, 125U, &exception);

        if (ret == AMP_MB_EXCEPTION) {
            fprintf(stderr, "MODBUS_EXCEPTION=%u\n", exception);
            return 1;
        }
        if (ret != AMP_MB_OK) {
            fprintf(stderr, "Invalid Modbus response: %d\n", ret);
            return 1;
        }

        if (request[1] == 3U) {
            unsigned int count =
                ((unsigned int)request[4] << 8) | request[5];
            for (i = 0; i < count; ++i)
                printf("REGISTER[%u]=%u\n",
                       ((((unsigned int)request[2] << 8) |
                          request[3]) + i), registers[i]);
        }
    } else {
        if (returned != length ||
            memcmp(response.payload + 20, payload + 12, length)) {
            fprintf(stderr, "Loopback data mismatch\n");
            return 1;
        }
    }

    puts(is_mock ? "PASS: software Modbus IPC transaction" :
                   "PASS: RTOS hardware loopback");
    return 0;
}
