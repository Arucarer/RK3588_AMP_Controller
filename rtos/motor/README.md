# 电机监督控制：第一版

本模块是独立、整数运算的状态机，不包含 FOC、模拟电机模型或硬件输出。
尚未加入 Makefile、RTOS 任务和 IPC；现有 LED、总线、启动及升级路径不变。

`amp_motor_init` 使用调用者提供的配置初始化为 DISABLED。测试参数仅用于软件
验证，不代表任何电机的额定参数。初始化后，配置和内部字段不应由其他模块直接
修改；由单个所有者调用 command/step，其他任务后续通过队列提交请求。

流程：初始化 → 健康且静止时 ENABLE → READY → 设置目标 → START → RUNNING。
STOP 将目标置零，按减速度降低指令，并等待有效反馈满足静止阈值后进入 READY。
反向运行先将指令减至零，再向反方向增加；这不保证真实转子已经停止。

监督逻辑以实际经过的毫秒数推进，当前仅接受 1..100 ms，不是高频电流环。
反馈年龄、过流、过温及控制租约超时会锁存故障，立即把软件需求置零。
DISABLE 不清除故障；RESET_FAULT 要求反馈健康且静止，复位后仍为 DISABLED。
必须重新使能及启动。只有显式 HEARTBEAT 刷新运行租约，目标更新不能代替心跳。

**软件需求为零不代表功率级已经关闭。** 后续设备接口必须另外执行硬件禁用、
处理禁用失败，并提供独立硬件保护。本版没有堵转、欠压、编码器诊断或安全认证。
过流/过温采用阈值立即触发；后续根据硬件设计添加去抖、持续时间与回差。

主机验证：

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/motor_control_test.c rtos/motor/motor_control.c \
    -o /tmp/amp_motor_control_test
/tmp/amp_motor_control_test
```

阅读顺序：motor_control.h（状态/接口）→ motor_control.c（转换/保护/斜坡）→
tests/motor_control_test.c（行为与边界）。后续新增模拟设备，再接 RTOS 队列，
最后扩展 IPC；每一阶段单独验证与提交。
