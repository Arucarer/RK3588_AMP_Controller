#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "amp_ipc.h"

#define CALL_TIMEOUT_MS 2000U

static int now_ms(uint64_t *value)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    *value = (uint64_t)ts.tv_sec * 1000U +
             (uint64_t)ts.tv_nsec / 1000000U;
    return 0;
}

static void poll_pause(void)
{
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000
    };

    nanosleep(&delay, NULL);
}

/* 返回 0：未超时；-1：超时或时钟读取失败。 */
static int check_timeout(uint64_t begin, unsigned int timeout)
{
    uint64_t now;

    if (now_ms(&now) != 0)
        return -1;

    if (now - begin >= timeout) {
        errno = ETIMEDOUT;
        return -1;
    }

    return 0;
}

static int random_session(uint64_t *session)
{
    int fd;
    size_t done = 0;
    unsigned char *bytes = (unsigned char *)session;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    while (done < sizeof(*session)) {
        ssize_t count = read(fd, bytes + done,
                             sizeof(*session) - done);

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0) {
            int saved = count == 0 ? EIO : errno;
            close(fd);
            errno = saved;
            return -1;
        }

        done += (size_t)count;
    }

    close(fd);

    if (*session == 0)
        *session = 1;

    return 0;
}

void amp_client_close(AMPClient *client)
{
    int saved = errno;

    if (client == NULL)
        return;

    if (client->transport != NULL) {
        munmap((void *)client->transport,
               AMP_TRANSPORT_MAP_SIZE);
        client->transport = NULL;
    }

    if (client->mem_fd >= 0) {
        close(client->mem_fd);
        client->mem_fd = -1;
    }

    if (client->lock_fd >= 0) {
        close(client->lock_fd);
        client->lock_fd = -1;
    }

    client->connected = 0;
    errno = saved;
}

int amp_client_call(AMPClient *client,
                    uint32_t opcode,
                    const void *payload,
                    uint32_t length,
                    AMPMessage *response)
{
    AMPMessage request = {0};
    AMPMessage incoming;
    uint64_t begin;
    int sent = 0;

    if (client == NULL || response == NULL ||
        length > AMP_MSG_PAYLOAD_MAX ||
        (length != 0 && payload == NULL)) {
        errno = EINVAL;
        return -1;
    }

    if (!client->connected || client->transport == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    if (client->sequence == UINT32_MAX) {
        client->connected = 0;
        errno = EOVERFLOW;
        return -1;
    }

    request.type = AMP_MSG_REQUEST;
    request.session = client->session;
    request.sequence = ++client->sequence;
    request.opcode = opcode;
    request.status = 0;
    request.payload_length = length;

    if (length != 0)
        memcpy(request.payload, payload, length);

    if (now_ms(&begin) != 0)
        goto failed;

    for (;;) {
        int result;

        if (check_timeout(begin, CALL_TIMEOUT_MS) != 0)
            goto failed;

        if (amp_transport_ready(client->transport) !=
            AMP_TRANSPORT_OK) {
            errno = EPROTO;
            goto failed;
        }

        /*
         * 始终消费返回通道：
         * - 清理旧会话的响应；
         * - 消费心跳，避免它阻塞命令响应；
         * - 查找当前请求的匹配响应。
         *
         * 不修改对端拥有的发布计数器。
         */
        result = amp_channel_receive(
            &client->transport->rtos_to_linux, &incoming);

        if (result == AMP_TRANSPORT_OK) {
            if (sent &&
                incoming.type == AMP_MSG_RESPONSE &&
                incoming.session == request.session &&
                incoming.sequence == request.sequence) {
                if (incoming.opcode != request.opcode) {
                    errno = EPROTO;
                    goto failed;
                }

                *response = incoming;
                return 0;
            }
        } else if (result != AMP_TRANSPORT_EMPTY) {
            errno = EBADMSG;
            goto failed;
        }

        if (!sent) {
            result = amp_channel_send(
                &client->transport->linux_to_rtos, &request);

            if (result == AMP_TRANSPORT_OK) {
                sent = 1;
            } else if (result != AMP_TRANSPORT_BUSY) {
                errno = EPROTO;
                goto failed;
            }
        }

        poll_pause();
    }

failed:
    /*
     * 超时时命令可能已经执行，只是响应未到。
     * 不自动生成新的请求序号重试。
     */
    client->connected = 0;
    return -1;
}

int amp_client_open(AMPClient *client)
{
    void *mapping;
    AMPMessage response;

    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 必须对新对象或已经 close 的对象调用。 */
    memset(client, 0, sizeof(*client));
    client->lock_fd = -1;
    client->mem_fd = -1;

    /*
     * 与旧测试程序共用锁文件。
     * 锁文件不要 unlink，否则可能出现两套独立锁。
     */
    client->lock_fd = open("/run/amp-ipc-test.lock",
                           O_CREAT | O_RDWR |
                           O_CLOEXEC | O_NOFOLLOW,
                           0600);
    if (client->lock_fd < 0)
        goto failed;

    if (flock(client->lock_fd, LOCK_EX | LOCK_NB) != 0)
        goto failed;

    client->mem_fd = open("/dev/mem",
                          O_RDWR | O_SYNC | O_CLOEXEC);
    if (client->mem_fd < 0)
        goto failed;

    mapping = mmap(NULL, AMP_TRANSPORT_MAP_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED,
                   client->mem_fd,
                   (off_t)AMP_TRANSPORT_ADDR);
    if (mapping == MAP_FAILED)
        goto failed;

    client->transport = (volatile AMPTransport *)mapping;

    if (amp_transport_ready(client->transport) !=
        AMP_TRANSPORT_OK) {
        errno = EPROTO;
        goto failed;
    }

    if (random_session(&client->session) != 0)
        goto failed;

    /*
     * connected 暂时允许发送 HELLO。
     * open 只有在 HELLO 应答成功后才返回成功。
     */
    client->connected = 1;

    if (amp_client_call(client, AMP_OP_HELLO,
                        NULL, 0, &response) != 0)
        goto failed;

    if (response.status != AMP_RESULT_OK ||
        response.payload_length != 0) {
        errno = EPROTO;
        goto failed;
    }

    return 0;

failed:
    amp_client_close(client);
    return -1;
}

int amp_client_heartbeat(AMPClient *client,
                         AMPMessage *message,
                         unsigned int timeout_ms)
{
    uint64_t begin;

    if (client == NULL || message == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!client->connected || client->transport == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    if (now_ms(&begin) != 0)
        goto failed;

    for (;;) {
        int result;

        if (check_timeout(begin, timeout_ms) != 0)
            goto failed;

        if (amp_transport_ready(client->transport) !=
            AMP_TRANSPORT_OK) {
            errno = EPROTO;
            goto failed;
        }

        result = amp_channel_receive(
            &client->transport->rtos_to_linux, message);

        if (result == AMP_TRANSPORT_OK) {
            if (message->session == client->session &&
                message->type == AMP_MSG_HEARTBEAT) {
                return 0;
            }
        } else if (result != AMP_TRANSPORT_EMPTY) {
            errno = EBADMSG;
            goto failed;
        }

        poll_pause();
    }

failed:
    client->connected = 0;
    return -1;
}