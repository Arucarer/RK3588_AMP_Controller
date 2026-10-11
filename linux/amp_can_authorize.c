#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amp_ipc.h"

#define CAN_AUTHORIZE_OPCODE 0x20U

static int parse(const char *s, unsigned long max, uint32_t *out)
{
    char *end;
    unsigned long value;

    if (!s || !*s || *s == '-')
        return -1;
    errno = 0;
    value = strtoul(s, &end, 0);
    if (errno || *end || value > max)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int main(int argc, char **argv)
{
    AMPClient client;
    AMPMessage response;
    uint8_t payload[80] = {0};
    uint32_t channel, id, flags;
    size_t length, i;

    if (argc != 6 ||
        parse(argv[1], 1, &channel) ||
        parse(argv[2], 0x1fffffffUL, &id) ||
        parse(argv[3], 7, &flags)) {
        fprintf(stderr,
                "usage: %s channel(0|1) id flags hex-data\n"
                "flags: extended=1, FD=2, BRS=4\n", argv[0]);
        return 2;
    }

    length = strlen(argv[4]);
    /* argv[5] pins the protocol version explicitly. */
    if (strcmp(argv[5], "v1") || length % 2 || length > 128) {
        fprintf(stderr, "invalid data or protocol version\n");
        return 2;
    }

    put32(payload, channel);
    put32(payload + 4, id);
    put32(payload + 8, flags);
    put32(payload + 12, (uint32_t)(length / 2));

    for (i = 0; i < length / 2; ++i) {
        char pair[3] = {argv[4][i * 2], argv[4][i * 2 + 1], 0};
        char *end;
        unsigned long value;

        if (!((pair[0] >= '0' && pair[0] <= '9') ||
              (pair[0] >= 'a' && pair[0] <= 'f') ||
              (pair[0] >= 'A' && pair[0] <= 'F')) ||
            !((pair[1] >= '0' && pair[1] <= '9') ||
              (pair[1] >= 'a' && pair[1] <= 'f') ||
              (pair[1] >= 'A' && pair[1] <= 'F')))
            return 2;
        value = strtoul(pair, &end, 16);
        if (*end)
            return 2;
        payload[16 + i] = (uint8_t)value;
    }

    if (amp_client_open(&client)) {
        perror("amp_client_open");
        return 1;
    }

    if (amp_client_call(&client, CAN_AUTHORIZE_OPCODE,
                        payload, sizeof(payload), &response)) {
        perror("amp_client_call");
        amp_client_close(&client);
        return 1;
    }
    amp_client_close(&client);

    if (response.opcode != CAN_AUTHORIZE_OPCODE ||
        response.status != AMP_RESULT_OK ||
        response.payload_length != sizeof(payload) ||
        memcmp(response.payload, payload, sizeof(payload))) {
        fprintf(stderr, "CAN authorization rejected: status=%u\n",
                response.status);
        return 1;
    }

    puts("AUTHORIZED: FreeRTOS CAN request validated");
    return 0;
}
