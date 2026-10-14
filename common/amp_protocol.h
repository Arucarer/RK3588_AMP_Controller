#ifndef AMP_PROTOCOL_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_PROTOCOL_H /* 定义头文件保护标记，避免本文件被重复包含 */

#include <stdint.h> /* 引入固定宽度整数类型，如 uint32_t、uint64_t */
#include <stddef.h> /* 引入 size_t、NULL 等基础类型与定义 */

#define AMP_IPC_PHYS_ADDR 0x20FD0000ULL /* 旧版 IPC 测试协议的共享内存物理地址 */
#define AMP_IPC_MAP_SIZE 4096U          /* 旧版 IPC 测试协议的映射大小，单位字节 */

#define AMP_IPC_MAGIC 0x414D5031U /* 旧版 IPC 测试协议的有效标记，对应 ASCII 字符 AMP1 */
#define AMP_IPC_VERSION 1U        /* 旧版 IPC 测试协议版本号 */

#define AMP_CMD_ADD 1U /* 旧版 IPC 测试操作码：计算两个无符号整数之和 */

#define AMP_STATUS_OK 0U          /* 旧版 IPC 测试结果：成功 */
#define AMP_STATUS_BAD_COMMAND 1U /* 旧版 IPC 测试结果：不支持的命令 */

/*
 * One outstanding request, one Linux client.
 *
 * Linux owns request fields.
 * RTOS owns response fields and initialization.
 * Sequence fields are published last.
 */
/* 旧版共享内存加法测试协议；请求和响应分别从 64 字节边界开始。 */
typedef struct {
    uint32_t magic;        /* 0x00：共享区有效标记，取 AMP_IPC_MAGIC */
    uint32_t version;      /* 0x04：旧版测试协议版本，取 AMP_IPC_VERSION */
    uint32_t reserved[14]; /* 协议头填充区，使请求区域从偏移 0x40 开始 */

    uint32_t request_seq;     /* 0x40：Linux 发布的请求序号 */
    uint32_t command;         /* 0x44：请求操作码，目前定义 AMP_CMD_ADD 加法命令 */
    uint32_t arg0;            /* 0x48：加法请求的第一个无符号操作数 */
    uint32_t arg1;            /* 0x4C：加法请求的第二个无符号操作数 */
    uint32_t request_pad[12]; /* 请求区填充，使响应区域从偏移 0x80 开始 */

    uint32_t response_seq;     /* 0x80：RTOS 已完成响应的请求序号 */
    uint32_t status;           /* 0x84：命令结果码，取 AMP_STATUS_* 常量 */
    uint32_t result;           /* 0x88：RTOS 返回的无符号加法结果 */
    uint32_t response_pad[13]; /* 响应区域的预留填充空间 */
} AMPSharedIPC;

_Static_assert(offsetof(AMPSharedIPC, request_seq) == 0x40, "Invalid request layout");
_Static_assert(offsetof(AMPSharedIPC, response_seq) == 0x80, "Invalid response layout");
_Static_assert(sizeof(AMPSharedIPC) == 0xC0, "Invalid IPC structure size");

#endif /* 结束 AMP_PROTOCOL_H 对应的条件编译区段 */
