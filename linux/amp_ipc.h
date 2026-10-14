/*
 * 文件名称：amp_ipc.h
 *
 * 模块名称：AMP IPC Linux 客户端接口
 *
 * 功能描述：
 * 本文件定义 RK3588 Linux + FreeRTOS AMP 架构下的 IPC 客户端接口，
 * 为 Linux 应用层提供统一的跨核通信能力，实现对 FreeRTOS 侧服务的
 * 命令调用、响应接收、会话管理和心跳检测。
 *
 * 核心功能：
 * 1. 客户端管理：完成 IPC 客户端的初始化、连接建立和资源释放。
 * 2. 共享内存通信：通过 AMPTransport 访问 Linux 与 FreeRTOS 的共享通信区域。
 * 3. 请求响应机制：发送操作码及负载数据，等待并获取匹配的响应消息。
 * 4. 会话管理：通过 Session 和 Sequence 标识通信会话及请求序列。
 * 5. 心跳检测：接收当前会话的心跳消息，辅助判断对端运行状态。
 * 6. 异常恢复：通信失败后关闭并重新打开客户端，重新建立通信状态。
 *
 * 设计特点：
 * 1. 将共享内存访问细节封装为统一的客户端 API，降低上层业务耦合。
 * 2. 区分 IPC 通信结果与 FreeRTOS 侧的实际命令执行结果。
 * 3. 同一 AMPClient 实例不支持多线程并发调用。
 * 4. 本文件仅声明数据结构及函数接口，具体通信逻辑由实现文件完成。
 *
 * 核心接口：
 * amp_client_open()       - 打开并初始化 IPC 客户端。
 * amp_client_close()      - 关闭客户端并释放相关资源。
 * amp_client_call()       - 发送命令并等待匹配响应。
 * amp_client_heartbeat()  - 等待当前会话的心跳消息。
 *
 * 适用场景：
 * RK3588 AMP 异构系统中，Linux 通过共享内存向 FreeRTOS 下发控制命令，
 * 并获取实时任务执行结果及系统运行状态。
 */
#ifndef AMP_IPC_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_IPC_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h>                  /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include "../common/amp_transport.h" /* 引入共享内存通道布局及收发接口 */

/* Linux IPC 客户端的映射、互斥锁及当前会话状态。 */
typedef struct {
    int lock_fd;                      //锁文件描述符
    int mem_fd;                       //内存设备文件描述符
    volatile AMPTransport *transport; //共享内存通信结构体指针
    uint64_t session;                 //当前通信会话编号
    uint32_t sequence;                //请求序列号
    int connected;                    //客户端连接状态
} AMPClient;

/* 初始化 IPC 客户端，建立 Linux 与 FreeRTOS 的通信会话。
 * client：客户端对象指针，成功后保存共享内存映射及会话信息。
 * 返回值：0 成功，-1 失败并设置 errno。 */
int amp_client_open(AMPClient *client);

/* 关闭 IPC 客户端，释放共享内存映射、文件描述符及锁资源。
 * client：需要关闭的客户端对象指针。 */
void amp_client_close(AMPClient *client);

/* 向 FreeRTOS 发送命令并等待匹配响应。
 * 返回值：0 收到匹配响应，-1 通信失败并设置 errno。
 * 注意：通信成功不代表命令执行成功，需检查 response->status。 */
int amp_client_call(AMPClient *client,     //保存当前 IPC 客户端的连接资源和状态
                    uint32_t opcode,       //是 Linux 发送给 FreeRTOS 的操作码
                    const void *payload,   //数据指针，指向 Linux 要发送的数据
                    uint32_t length,       //发送数据的长度
                    AMPMessage *response); //表示这次IPC的调用在通讯层是否成功

/* 等待 FreeRTOS 主动发送的当前会话心跳。
 * 返回值：0 收到心跳，-1 超时或其他错误并设置 errno。 */
int amp_client_heartbeat(AMPClient *client,        //当前已经打开的 IPC 客户端
                         AMPMessage *message,      //保存收到的心跳消息
                         unsigned int timeout_ms); //最长等待时间

#endif /* 结束 AMP_IPC_H 对应的条件编译区段 */
