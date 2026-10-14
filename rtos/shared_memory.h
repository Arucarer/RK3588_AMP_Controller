#ifndef AMP_SHARED_MEMORY_H /* 头文件保护：未定义此标记时才包含下面的内容 */
#define AMP_SHARED_MEMORY_H /* 定义头文件保护标记，避免本文件被重复包含 */

/* 初始化共享传输区和 IPC 会话状态；准备请求缓存及通信计数。 */
void amp_ipc_init(void);
/* 循环收集异步任务结果并接收 Linux 请求；执行校验及命令分发，发送响应；周期发布心跳并上报 IPC 任务存活状态。 */
void amp_ipc_task(void *argument);

#endif /* 结束 AMP_SHARED_MEMORY_H 对应的条件编译区段 */
