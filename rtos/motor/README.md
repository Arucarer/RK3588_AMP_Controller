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

## 第四版：接入构建、启动与健康监测

现有Makefile现在编译三个motor模块；main在调度器启动前创建MotorSIM，失败
沿用任务初始化失败路径。启动后模拟任务自动推进，但控制器仍为DISABLED，
没有自动使能、转动或访问PWM/ADC/CAN/UART。现有LED、总线命令和IPC格式不变。
电机IPC、Linux命令行及板端部署是后续独立任务；当前没有外部电机控制入口。

monitor_require_motor在创建任务时登记心跳要求。每完成一次模拟周期后上报
MONITOR_MOTOR；健康ready必须等到它至少完成一次周期。超过1000ms未上报
（沿用旧监测规则，严格大于）产生MONITOR_FAULT_MOTOR=0x10并锁存。恢复心跳
只能清除当前故障，不能清除历史故障；现有Linux健康服务会拒绝非零故障。

模拟过流、过温、命令失联等是业务故障，任务仍存活时不会设置任务超时位。
10ms周期超期计数保留在电机快照中，不等同于已经满足高频实时控制要求。
MonitorSnapshot仍为32字节，旧健康消息各字段偏移不变；没有新增motor_age字段。

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/support/motor_rtos tests/monitor_motor_test.c rtos/monitor_task.c \
    -o /tmp/amp_monitor_motor_test
/tmp/amp_monitor_motor_test
```

以上主机测试及隔离副本完整固件构建通过不代表已上机。板端任务周期、堆/栈
余量、原业务并发及A/B健康确认仍需配套发布后的验收；本阶段不刷写开发板。

## 第五版：电机IPC扩展

新增STATUS=0x40和EXEC=0x41，V1负载定义见common/amp_motor_protocol.h。
STATUS请求为0字节；EXEC为32字节，响应为64字节。小端逐字段编码，明确SIM=1，
不复制C结构体。负转速、电流、温度和业务结果使用32位补码编码；Tick为64位。
外层status表示IPC接受/拒绝，负载result表示电机命令接受结果，均不代表动作完成。
不改变旧GET_STATUS、LED和BUS的消息布局。

EXEC支持使能、禁用、启动、停止、目标、心跳、复位、SIM故障注入。IpcTask先
执行原有会话/序号检查和重复请求缓存，再调用桥接；异步任务完成后才缓存回复。
等待电机结果期间不消费新请求，不切换HELLO会话；重复心跳重放缓存，不延长租约。

控制令牌为独立非零64位值，与每次短连接的IPC session不同。Linux工具后续应
跨交易保存令牌，每次交易只短暂占用现有IPC锁，不能长期占锁阻塞健康服务。
令牌是协作式所有权标识，不是身份认证；当前/dev/mem访问权限仍由Linux管理。
状态查询不占有电机、不续租。其他令牌不能修改运行中/停止中的控制器。新令牌
可显式使能DISABLED、禁用READY/DISABLED，或在FAULT时请求复位；实际复位仍需
MotorSIM确认反馈健康且静止。成功禁用/复位释放令牌；失败不转移控制权。
控制端消失时由已有运行租约超时锁存故障，不自动重启电机。

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/motor_ipc_test.c rtos/motor/motor_ipc.c common/amp_message.c \
    -o /tmp/amp_motor_ipc_test
/tmp/amp_motor_ipc_test
```

桥接测试使用任务队列替身，不等于共享内存物理链路已上机。Linux命令行工具、
板端回归及业务闭环验收仍未完成，本阶段不部署固件。

## 第六版：Linux MotorSIM命令行

新增linux/amp_motorctl.c和tools/build_motorctl.sh。run命令完成启停及周期心跳，
独立命令支持状态、目标、故障注入和复位。令牌文件跨短IPC会话保持控制权，
每次交易结束关闭IPC连接，避免运行期间长期占用全局IPC锁。
仅重试获取忙锁，不自动重放结果未知的命令；异常后尽力禁用，不能证明物理停机。
详细用法和限制见linux/scripts/motorctl-usage.md。

主机集成测试：`python3 tests/motor_cli_test.py`。11项用例及带地址/未定义行为
检查的模拟运行通过；ARM64静态编译通过。测试中的时钟、IPC设备和任务队列为
替身，尚未进行板端RTOS调度、IPC争用、看门狗与HMI并发验收。不刷写开发板。

## 第七版：离线交付准备，板端验收延期

tools/prepare_motor_sim.py从当前工作区复制构建快照，在临时目录中编译RTOS和
Linux工具，向全新目录输出配套镜像、ELF/MAP、编译日志、源码/产物清单及哈希。
原有构建产物、Git改动和SDK保持不变。输出不是完整签名OTA，不包含boot/rootfs，
也不连接开发板。详细步骤见linux/scripts/motor-offline-handoff.md。

当前模拟电机的软件实现和主机检查已完成，板端验收按用户安排延期，未标记通过。
真实电机FOC、同步PWM/采样、编码器、硬件保护及实时性能仍属于后续开发与验证。
