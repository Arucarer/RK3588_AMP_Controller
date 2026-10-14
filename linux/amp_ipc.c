/*
 * Linux AMP IPC 客户端
 * 负责共享内存映射、独占访问、会话建立、消息收发和心跳检测。
 */

#define _POSIX_C_SOURCE 200809L /* 启用 POSIX.1-2008 接口声明，如单调时钟和纳秒休眠 */

#include <errno.h>    /* 引入 errno 和系统调用错误码 */
#include <fcntl.h>    /* 引入文件打开选项与文件控制接口 */
#include <stdint.h>   /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include <string.h>   /* 引入内存复制、清零及字符串处理接口 */
#include <sys/file.h> /* 引入 flock 文件锁接口 */
#include <sys/mman.h> /* 引入 mmap、munmap 及内存映射选项 */
#include <time.h>     /* 引入时钟读取、时间结构及休眠接口 */
#include <unistd.h>   /* 引入 POSIX 文件读写、关闭及其他系统接口 */

#include "amp_ipc.h" /* 引入 Linux IPC 客户端接口 */

#define CALL_TIMEOUT_MS 2000U /* 单次 IPC 请求响应的等待超时，单位 ms */

/* 获取单调时钟的毫秒时间 */
static int now_ms(uint64_t *value)
{
    /* 1. 读取单调时钟，避免系统时间调整影响超时。 */
    /* 2. 换算为接口使用的毫秒或纳秒单位。 */

    struct timespec ts; //定义时间结构体

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) //获取当前单调时钟时间
        return -1;

    *value = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U; //转换成毫秒
    return 0;
}

/* 轮询休眠 1ms，减少 CPU 占用 */
static void poll_pause(void)
{
    /* 1. 短暂休眠，降低 IPC 轮询对 CPU 的占用。 */

    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};

    nanosleep(&delay, NULL);
}

/* 检查通信是否超时 */
static int check_timeout(uint64_t begin, unsigned int timeout) //输入开始时间与允许等待的最大时间
{
    /* 1. 比较当前单调时间与截止时间。 */
    /* 2. 超时则设置错误信息并返回失败。 */

    uint64_t now;

    //调用获取单调时钟的时间，如果获取失败就会返回-1
    if (now_ms(&now) != 0)
        return -1;

    if (now - begin >= timeout) {
        errno = ETIMEDOUT;
        return -1;
    }

    return 0;
}

/* 生成随机会话 ID */
static int random_session(uint64_t *session)
{
    /* 1. 读取系统随机源生成会话号。 */
    /* 2. 保证有效会话号非零并处理随机源读取失败。 */

    int fd;
    size_t done = 0;
    unsigned char *bytes = (unsigned char *)session;

    /* 1. 打开系统随机数设备 */
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    /* 2. 读取 64 位随机数，处理读取中断和短读 */
    while (done < sizeof(*session)) {
        ssize_t count = read(fd, bytes + done, sizeof(*session) - done);
        /* 被信号中断时重试读取，其他读取错误交由下面的失败路径处理。 */
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

    /* 3. 关闭设备，确保会话 ID 非零 */
    close(fd);

    if (*session == 0)
        *session = 1;

    return 0;
}

/* 关闭 IPC 客户端，释放相关资源 */
void amp_client_close(AMPClient *client)
{
    /* 1. 解除共享内存映射。 */
    /* 2. 关闭内存设备及进程锁描述符。 */
    /* 3. 清理客户端连接状态。 */

    //保存错误码
    int saved = errno;

    if (client == NULL)
        return;

    /* 1. 解除共享内存映射 */
    if (client->transport != NULL) {
        munmap((void *)client->transport, AMP_TRANSPORT_MAP_SIZE);
        client->transport = NULL;
    }

    /* 2. 关闭物理内存设备 */
    if (client->mem_fd >= 0) {
        close(client->mem_fd);
        client->mem_fd = -1;
    }

    /* 3. 关闭锁文件，释放独占锁 */
    if (client->lock_fd >= 0) {
        close(client->lock_fd);
        client->lock_fd = -1;
    }

    /* 4. 重置连接状态，保留原始错误码 */
    client->connected = 0;
    errno = saved;
}

/* 发送 IPC 请求并等待对应响应 */
int amp_client_call(AMPClient *client, uint32_t opcode, const void *payload, uint32_t length,
                    AMPMessage *response)
{
    /* 1. 检查连接、操作码和负载，构造带会话及序号的请求。 */
    /* 2. 发送后轮询响应，过滤其他会话或不匹配消息。 */
    /* 3. 返回匹配响应，超时或传输错误时报告失败。 */

    AMPMessage request = {0};
    AMPMessage incoming;
    uint64_t begin;
    int sent = 0;

    /* 1. 检查参数、连接状态和请求序号 */
    if (client == NULL || response == NULL || length > AMP_MSG_PAYLOAD_MAX ||
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

    /* 2. 构造请求消息，设置会话 ID、序号和命令参数 */
    request.type = AMP_MSG_REQUEST;
    request.session = client->session; // 本次通信会话
    request.sequence = ++client->sequence;
    request.opcode = opcode;
    request.status = 0;
    request.payload_length = length;

    if (length != 0)
        memcpy(request.payload, payload, length);

    /* 3. 记录开始时间，进入通信轮询 */
    if (now_ms(&begin) != 0)
        goto failed;

    for (;;) {
        int result;

        /* 4. 检查通信是否超时及共享内存是否就绪 */
        if (check_timeout(begin, CALL_TIMEOUT_MS) != 0)
            goto failed;

        if (amp_transport_ready(client->transport) != AMP_TRANSPORT_OK) {
            errno = EPROTO;
            goto failed;
        }

        /* 5. 接收 RTOS 消息，检查是否为当前请求的响应 */
        result = amp_channel_receive(&client->transport->rtos_to_linux, &incoming);

        if (result == AMP_TRANSPORT_OK) {
            if (sent && incoming.type == AMP_MSG_RESPONSE && incoming.session == request.session &&
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

        /* 6. 请求尚未发送时，尝试写入 Linux → RTOS 通道 */
        if (!sent) {
            result = amp_channel_send(&client->transport->linux_to_rtos, &request);

            /* 成功后标记已发送；通道忙时短暂等待后重试，其他传输错误立即失败。 */
            if (result == AMP_TRANSPORT_OK) {
                sent = 1;
            } else if (result != AMP_TRANSPORT_BUSY) {
                errno = EPROTO;
                goto failed;
            }
        }

        /* 7. 休眠 1ms，继续等待响应 */
        poll_pause();
    }

failed:
    /* 8. 通信失败后断开会话，不自动重发可能已执行的命令 */
    client->connected = 0;
    return -1;
}

/* 初始化 IPC 客户端并建立通信会话 */
int amp_client_open(AMPClient *client)
{
    /* 1. 初始化客户端并获取独占锁。 */
    /* 2. 映射共享区，核对协议就绪状态。 */
    /* 3. 建立随机会话并通过 HELLO 握手确认连接。 */

    void *mapping;
    AMPMessage response;

    /* 1. 检查客户端指针并初始化状态 */
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(client, 0, sizeof(*client));
    client->lock_fd = -1;
    client->mem_fd = -1;

    /* 2. 创建锁文件并获取独占锁，防止多个客户端竞争 */
    client->lock_fd =
        open("/run/amp-ipc-test.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (client->lock_fd < 0)
        goto failed;

    //尝试获取文件独占所，防止多个Linux客户端同时访问AMP贡献内存通信通道
    if (flock(client->lock_fd, LOCK_EX | LOCK_NB) != 0)
        goto failed;

    /* 3. 打开 /dev/mem，映射 AMP 物理共享内存 */
    client->mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (client->mem_fd < 0)
        goto failed;

    /* 将 AMP IPC 物理共享内存映射到 Linux 进程的虚拟地址空间 */
    mapping = mmap(NULL,                       // 由系统自动分配虚拟地址
                   AMP_TRANSPORT_MAP_SIZE,     // 映射的内存大小
                   PROT_READ | PROT_WRITE,     // 允许读写
                   MAP_SHARED,                 // 共享映射
                   client->mem_fd,             // /dev/mem 文件描述符
                   (off_t)AMP_TRANSPORT_ADDR); // 共享内存物理起始地址

    if (mapping == MAP_FAILED)
        goto failed;

    //将 mmap() 返回的虚拟地址转换为 AMPTransport 结构体指针，并保存到 client->transport 中
    client->transport = (volatile AMPTransport *)mapping;

    /* 4. 检查共享内存通信区是否初始化完成 */
    if (amp_transport_ready(client->transport) != AMP_TRANSPORT_OK) {
        errno = EPROTO;
        goto failed;
    }

    /* 5. 生成随机会话 ID */
    if (random_session(&client->session) != 0)
        goto failed;

    /* 6. 临时标记连接有效，发送 HELLO 请求建立会话 */
    client->connected = 1;

    if (amp_client_call(client, AMP_OP_HELLO, NULL, 0, &response) != 0)
        goto failed;

    /* 7. 检查 RTOS 返回的 HELLO 响应 */
    if (response.status != AMP_RESULT_OK || response.payload_length != 0) {
        errno = EPROTO;
        goto failed;
    }

    return 0;

failed:
    /* 8. 初始化失败时统一释放资源 */
    amp_client_close(client);
    return -1;
}

/* 接收 FreeRTOS 心跳，检查当前通信会话 */
int amp_client_heartbeat(AMPClient *client, AMPMessage *message, unsigned int timeout_ms)
{
    /* 1. 检查连接并设置等待期限。 */
    /* 2. 轮询接收消息，筛选当前会话的心跳。 */
    /* 3. 返回匹配心跳或超时错误。 */

    uint64_t begin;

    /* 1. 检查参数及客户端连接状态 */
    if (client == NULL || message == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!client->connected || client->transport == NULL) {
        errno = ENOTCONN;
        return -1;
    }

    /* 2. 记录心跳等待开始时间 */
    if (now_ms(&begin) != 0)
        goto failed;

    for (;;) {
        int result;

        /* 3. 检查等待是否超时及共享内存是否就绪 */
        if (check_timeout(begin, timeout_ms) != 0)
            goto failed;

        if (amp_transport_ready(client->transport) != AMP_TRANSPORT_OK) {
            errno = EPROTO;
            goto failed;
        }

        /* 4. 从 RTOS → Linux 通道读取消息 */
        result = amp_channel_receive(&client->transport->rtos_to_linux, message);

        /* 5. 验证消息是否为当前会话的心跳 */
        if (result == AMP_TRANSPORT_OK) {
            if (message->session == client->session && message->type == AMP_MSG_HEARTBEAT) {
                return 0;
            }
        } else if (result != AMP_TRANSPORT_EMPTY) {
            errno = EBADMSG;
            goto failed;
        }

        /* 6. 休眠 1ms，继续轮询心跳 */
        poll_pause();
    }

failed:
    /* 7. 心跳超时或通信异常，标记连接失效 */
    client->connected = 0;
    return -1;
}
