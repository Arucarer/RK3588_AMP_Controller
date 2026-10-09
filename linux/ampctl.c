#include <errno.h>
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

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int parse_period(const char *text, uint32_t *value)
{
    const char *p;
    char *end;
    unsigned long number;

    if (text == NULL || *text == '\0')
        return -1;

    for (p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    number = strtoul(text, &end, 10);

    if (errno != 0 || *end != '\0' ||
        number < 100UL || number > 10000UL ||
        number % 20UL != 0) {
        return -1;
    }

    *value = (uint32_t)number;
    return 0;
}

static int check_response(const AMPMessage *message)
{
    if (message->status == AMP_RESULT_OK)
        return 0;

    fprintf(stderr,
            "Remote error: opcode=%" PRIu32
            ", status=%" PRIu32 "\n",
            message->opcode, message->status);

    return -1;
}


static int print_status(const AMPMessage *message)
{
    const uint8_t *p = message->payload;
    uint32_t version;

    if (check_response(message) != 0)
        return -1;

    if (message->opcode != AMP_OP_GET_STATUS ||
        message->payload_length < 4U) {
        fprintf(stderr, "Invalid status message\n");
        return -1;
    }

    version = get_u32(p);

    /* 新客户端同时支持旧状态和新状态。 */
    if (!((version == 1U && message->payload_length == 32U) ||
          (version == 2U && message->payload_length == 64U))) {
        fprintf(stderr, "Unsupported status layout\n");
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

    if (version == 2U) {
        printf(
            "  HEALTH ready=%" PRIu32
            " faults=0x%08" PRIx32
            " latched=0x%08" PRIx32
            " checks=%" PRIu32
            " ipc_age_ms=%" PRIu32
            " control_age_ms=%" PRIu32
            " monitor_age_ms=%" PRIu32
            " led_fault=%" PRIu32 "\n",
            get_u32(p + 32),
            get_u32(p + 36),
            get_u32(p + 40),
            get_u32(p + 44),
            get_u32(p + 48),
            get_u32(p + 52),
            get_u32(p + 56),
            get_u32(p + 60));
    } else {
        printf("  HEALTH unavailable: old status format\n");
    }

    fflush(stdout);
    return 0;
}

static int print_led(const AMPMessage *message)
{
    uint32_t mode;
    uint32_t output;
    uint32_t period;
    const char *name;

    if (check_response(message) != 0)
        return -1;

    if ((message->opcode != AMP_OP_LED_SET &&
         message->opcode != AMP_OP_LED_GET) ||
        message->payload_length != 12U) {
        fprintf(stderr, "Invalid LED response\n");
        return -1;
    }

    mode = get_u32(message->payload);
    output = get_u32(message->payload + 4);
    period = get_u32(message->payload + 8);

    if (output > 1U) {
        fprintf(stderr, "Invalid LED output value\n");
        return -1;
    }

    switch (mode) {
    case AMP_LED_OFF:
        name = "OFF";
        break;

    case AMP_LED_ON:
        name = "ON";
        break;

    case AMP_LED_BLINK:
        name = "BLINK";
        break;

    default:
        fprintf(stderr, "Invalid LED mode\n");
        return -1;
    }

    if (mode == AMP_LED_BLINK) {
        if (period < 100U || period > 10000U ||
            period % 20U != 0) {
            fprintf(stderr, "Invalid LED period\n");
            return -1;
        }
    } else if (period != 0U) {
        fprintf(stderr, "Unexpected LED period\n");
        return -1;
    }

    printf("LED mode=%s output=%" PRIu32
           " period_ms=%" PRIu32
           " session=%016" PRIx64
           " seq=%" PRIu32 "\n",
           name, output, period,
           message->session, message->sequence);

    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s ping [text]\n"
            "  %s status\n"
            "  %s monitor\n"
            "  %s led on\n"
            "  %s led off\n"
            "  %s led blink <period_ms>\n"
            "  %s led status\n"
            "\n"
            "Blink period: 100..10000 ms, multiple of 20.\n",
            program, program, program, program,
            program, program, program);
}

int main(int argc, char **argv)
{
    enum {
        MODE_PING,
        MODE_STATUS,
        MODE_MONITOR,
        MODE_LED_SET,
        MODE_LED_GET
    } mode;

    AMPClient client;
    AMPMessage response;
    const char *text = "hello";
    uint8_t led_payload[8];
    uint32_t led_mode = AMP_LED_OFF;
    uint32_t period_ms = 0;
    int exit_code = EXIT_FAILURE;

    if (argc >= 2 && strcmp(argv[1], "ping") == 0 &&
        (argc == 2 || argc == 3)) {
        mode = MODE_PING;

        if (argc == 3)
            text = argv[2];

        if (strlen(text) > AMP_MSG_PAYLOAD_MAX) {
            fprintf(stderr, "Ping text exceeds %u bytes\n",
                    (unsigned int)AMP_MSG_PAYLOAD_MAX);
            return EXIT_FAILURE;
        }
    } else if (argc == 2 &&
               strcmp(argv[1], "status") == 0) {
        mode = MODE_STATUS;
    } else if (argc == 2 &&
               strcmp(argv[1], "monitor") == 0) {
        mode = MODE_MONITOR;
    } else if (argc >= 3 &&
               strcmp(argv[1], "led") == 0) {
        if (argc == 3 && strcmp(argv[2], "on") == 0) {
            mode = MODE_LED_SET;
            led_mode = AMP_LED_ON;
        } else if (argc == 3 &&
                   strcmp(argv[2], "off") == 0) {
            mode = MODE_LED_SET;
            led_mode = AMP_LED_OFF;
        } else if (argc == 3 &&
                   strcmp(argv[2], "status") == 0) {
            mode = MODE_LED_GET;
        } else if (argc == 4 &&
                   strcmp(argv[2], "blink") == 0) {
            mode = MODE_LED_SET;
            led_mode = AMP_LED_BLINK;

            if (parse_period(argv[3], &period_ms) != 0) {
                fprintf(stderr,
                        "Period must be 100..10000 ms "
                        "and a multiple of 20.\n");
                return EXIT_FAILURE;
            }
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (amp_client_open(&client) != 0) {
        perror("IPC connect");
        return EXIT_FAILURE;
    }

    switch (mode) {
    case MODE_PING:
    {
        uint32_t length = (uint32_t)strlen(text);

        if (amp_client_call(&client, AMP_OP_PING,
                            text, length, &response) != 0) {
            perror("PING");
            break;
        }

        if (check_response(&response) != 0)
            break;

        if (response.payload_length != length ||
            memcmp(response.payload, text, length) != 0) {
            fprintf(stderr, "PING payload mismatch\n");
            break;
        }

        printf("PONG session=%016" PRIx64
               " seq=%" PRIu32 " text=%s\n",
               response.session, response.sequence, text);

        exit_code = EXIT_SUCCESS;
        break;
    }

    case MODE_STATUS:
        if (amp_client_call(&client, AMP_OP_GET_STATUS,
                            NULL, 0, &response) != 0) {
            perror("GET_STATUS");
            break;
        }

        if (print_status(&response) == 0)
            exit_code = EXIT_SUCCESS;
        break;

    case MODE_MONITOR:
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

    case MODE_LED_SET:
        put_u32(led_payload, led_mode);
        put_u32(led_payload + 4, period_ms);

        if (amp_client_call(&client, AMP_OP_LED_SET,
                            led_payload, sizeof(led_payload),
                            &response) != 0) {
            perror("LED_SET");
            break;
        }

        if (print_led(&response) == 0)
            exit_code = EXIT_SUCCESS;
        break;

    case MODE_LED_GET:
        if (amp_client_call(&client, AMP_OP_LED_GET,
                            NULL, 0, &response) != 0) {
            perror("LED_GET");
            break;
        }

        if (print_led(&response) == 0)
            exit_code = EXIT_SUCCESS;
        break;
    }

out:
    amp_client_close(&client);
    return exit_code;
}