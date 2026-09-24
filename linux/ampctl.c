#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amp_ipc.h"

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int print_status(const AMPMessage *message)
{
    const uint8_t *p = message->payload;

    if (message->opcode != AMP_OP_GET_STATUS ||
        message->status != AMP_RESULT_OK ||
        message->payload_length != 32U ||
        get_u32(p) != 1U) {
        fprintf(stderr, "Invalid status message\n");
        return -1;
    }

    printf(
        "%s session=%016" PRIx64 " seq=%" PRIu32
        " tick=%" PRIu32 " hz=%" PRIu32
        " requests=%" PRIu32 " duplicates=%" PRIu32
        " bad_frames=%" PRIu32 " rejected=%" PRIu32
        " last_request=%" PRIu32 "\n",
        message->type == AMP_MSG_HEARTBEAT ?
            "HEARTBEAT" : "STATUS",
        message->session,
        message->sequence,
        get_u32(p + 4),
        get_u32(p + 8),
        get_u32(p + 12),
        get_u32(p + 16),
        get_u32(p + 20),
        get_u32(p + 24),
        get_u32(p + 28));

    fflush(stdout);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s ping [text]\n"
            "  %s status\n"
            "  %s monitor\n",
            program, program, program);
}

int main(int argc, char **argv)
{
    AMPClient client;
    AMPMessage response;
    const char *text = "hello";
    int mode;
    int exit_code = EXIT_FAILURE;

    if (argc >= 2 && strcmp(argv[1], "ping") == 0 &&
        (argc == 2 || argc == 3)) {
        mode = 1;

        if (argc == 3)
            text = argv[2];

        if (strlen(text) > AMP_MSG_PAYLOAD_MAX) {
            fprintf(stderr, "Ping text exceeds %u bytes\n",
                    (unsigned int)AMP_MSG_PAYLOAD_MAX);
            return EXIT_FAILURE;
        }
    } else if (argc == 2 &&
               strcmp(argv[1], "status") == 0) {
        mode = 2;
    } else if (argc == 2 &&
               strcmp(argv[1], "monitor") == 0) {
        mode = 3;
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (amp_client_open(&client) != 0) {
        perror("IPC connect");
        fprintf(stderr,
                "Check RTOS startup, protocol version "
                "and whether another client is running.\n");
        return EXIT_FAILURE;
    }

    if (mode == 1) {
        uint32_t length = (uint32_t)strlen(text);

        if (amp_client_call(&client, AMP_OP_PING,
                            text, length, &response) != 0) {
            perror("PING");
            goto out;
        }

        if (response.status != AMP_RESULT_OK ||
            response.payload_length != length ||
            memcmp(response.payload, text, length) != 0) {
            fprintf(stderr,
                    "PING failed: status=%" PRIu32 "\n",
                    response.status);
            goto out;
        }

        printf("PONG session=%016" PRIx64
               " seq=%" PRIu32 " text=%s\n",
               response.session, response.sequence, text);

        exit_code = EXIT_SUCCESS;
    } else if (mode == 2) {
        if (amp_client_call(&client, AMP_OP_GET_STATUS,
                            NULL, 0, &response) != 0) {
            perror("GET_STATUS");
            goto out;
        }

        if (print_status(&response) == 0)
            exit_code = EXIT_SUCCESS;
    } else {
        printf("Monitoring heartbeat; press Ctrl+C to exit.\n");
        fflush(stdout);

        for (;;) {
            if (amp_client_heartbeat(&client,
                                     &response, 3000U) != 0) {
                perror("HEARTBEAT");
                goto out;
            }

            if (print_status(&response) != 0)
                goto out;
        }
    }

out:
    amp_client_close(&client);
    return exit_code;
}