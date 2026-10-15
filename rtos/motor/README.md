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
tests/motor_control_test.c（行为与边界）。模拟设备见下文；后续接 RTOS 队列，
再扩展 IPC，每一阶段单独验证与提交。

## 第二版：SIM 行为模型

`motor_sim.h/.c` 提供确定性的整数模型：转速一阶响应、负载降速和电流增加、
电流相关升温、撤销使能后的滑行及冷却。它不包含电阻、电感、磁链、三相电流
或转子动力学，不能用于 FOC 数值验证、控制器整定或真实电机性能预测。
输入必须来自已校验的目标；无硬件模式、无真实功率输出。

注入支持堵转、反馈丢失和合成过温反馈。堵转时本模型把速度强制为零并提高
电流，联合测试通过现有过流保护停机；这不是已经实现独立的堵转识别算法。
反馈丢失时保持最后发布的数值并累加 age_ms，内部模型仍继续运行；恢复时发布
最新反馈并清零年龄。年龄饱和到 UINT32_MAX，不会回绕。过温注入改变传感器
反馈，不修改内部热状态；清除后由正常反馈决定能否显式复位。

联合调用顺序：读取快照 → 状态机 step → 将需求和使能状态交给模拟设备 step。
故障会撤销下一步模拟设备使能，但真实硬件仍需独立的禁用与故障关断接口。
启动参数和模型响应常数均为测试示例，不是已选电机参数。

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/motor_sim_test.c rtos/motor/motor_sim.c rtos/motor/motor_control.c \
    -o /tmp/amp_motor_sim_test
/tmp/amp_motor_sim_test
```

本阶段仍未改动现有 Makefile、main、IPC 或板端固件；主机验证不代表实时任务
已在开发板运行。下一阶段再添加默认仅SIM运行的独立任务和队列。

## 第三版：独立 MotorSIM 任务与队列接口

`motor_task.h/.c` 创建一个10ms周期、优先级2、栈深1024个元素的SIM任务。
当前适配100Hz Tick，按实际经过的Tick推进；迟到时重置下次唤醒时间，不连续
补算控制步。超过100ms的间隔触发已使能控制器的时序故障，并使反馈无效，
不把软件需求归零当作转子已停止的证据。过期后排队的心跳不能覆盖已有超时。

三个深度1队列分别承担命令、回复和最新快照。只有任务拥有控制器和模拟设备。
单个客户端submit/receive，每次只能有一个在途请求；取回回复前持续返回忙。
快照通过overwrite/peek发布，查询不消耗快照。回复保留session和sequence，
但尚未实现IPC会话授权、去重或在线配置。command回复只表示接受结果。

所有快照simulation固定为1，初始状态为DISABLED。无自动使能、无自动心跳、
无真实硬件后端。故障注入仅操作内存中的模拟模型。低频SIM任务不是FOC任务。
下一阶段需要增加IPC入口、main初始化、构建源列表及健康监测，再考虑板端部署。
当前只提供任务实现，不在现有启动路径创建它，已有调度与命令保持不变。

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/support/motor_rtos tests/motor_task_test.c \
    rtos/motor/motor_task.c rtos/motor/motor_control.c rtos/motor/motor_sim.c \
    -o /tmp/amp_motor_task_test
/tmp/amp_motor_task_test
```

该测试使用确定性FreeRTOS API替身运行实际任务循环，验证队列、失败清理、
命令回复、快照、租约、调度超期、Tick回绕及注入；不验证真实FreeRTOS调度、
抢占、堆/栈余量或板端10ms周期。替身头文件只用于主机测试，禁止用于固件。
