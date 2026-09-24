#ifndef AMP_IPC_H
#define AMP_IPC_H

#include <stdint.h>
#include "../common/amp_transport.h"

typedef struct {
    int lock_fd;
    int mem_fd;
    volatile AMPTransport *transport;
    uint64_t session;
    uint32_t sequence;
    int connected;
} AMPClient;

/*
 * 同一个 AMPClient 不支持多线程同时调用。
 * 返回 0 表示成功，-1 表示失败并设置 errno。
 */
int amp_client_open(AMPClient *client);
void amp_client_close(AMPClient *client);

/*
 * 返回 0 表示收到匹配响应。
 * 对端执行是否成功还需检查 response->status。
 *
 * 通信失败后，必须关闭并重新打开客户端。
 */
int amp_client_call(AMPClient *client,
                    uint32_t opcode,
                    const void *payload,
                    uint32_t length,
                    AMPMessage *response);

/* 等待当前会话的心跳。 */
int amp_client_heartbeat(AMPClient *client,
                         AMPMessage *message,
                         unsigned int timeout_ms);

#endif