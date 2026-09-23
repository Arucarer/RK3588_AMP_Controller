# RK3588 AMP Controller

基于 Rockchip RK3588 的 Linux + FreeRTOS AMP 异构多核实时控制系统。

本项目面向车载域控制器、工业控制器、机器人控制器以及边缘实时控制等场景，研究并实现 RK3588 平台下的 **AMP（Asymmetric Multiprocessing，非对称多处理）双系统架构**。

RK3588 采用 8 核 ARM64 异构 CPU 架构，包括：

* 4 × Cortex-A55
* 4 × Cortex-A76

项目采用 Linux 与 FreeRTOS 分核运行的方式：Linux 继续保持 SMP 多核调度，负责网络、交互、数据显示以及复杂业务；单独划分一个 Cortex-A55 核运行 FreeRTOS，负责实时采集、实时控制和低延迟任务。

第一版计划将：

```text
cpu_l3
Cortex-A55
MPIDR = 0x300
```

作为 FreeRTOS 独立运行核心。

---

# 一、项目背景

传统 Linux 系统虽然拥有完善的驱动、网络、文件系统和应用生态，但普通 Linux 并不能保证严格的实时响应。

FreeRTOS 等 RTOS 则具有调度路径短、任务优先级明确、中断响应稳定等特点，适合传感器采集、电机控制、GPIO 中断、现场总线控制等实时任务。

因此，本项目采用 AMP 架构，将 Linux 与 FreeRTOS 部署在不同 CPU 核心：

```text
                    RK3588

        ┌─────────────────────────┐
        │                         │
        │      Cortex-A76/A55     │
        │                         │
        │     Debian Linux        │
        │       SMP 系统           │
        │                         │
        │ 网络 / 显示 / 存储       │
        │ 用户交互 / 上层业务       │
        │                         │
        └───────────┬─────────────┘
                    │
             IPC / Shared Memory
             Mailbox / RPMsg
                    │
        ┌───────────▼─────────────┐
        │                         │
        │      Cortex-A55         │
        │       cpu_l3            │
        │     MPIDR 0x300         │
        │                         │
        │       FreeRTOS          │
        │                         │
        │  实时采集 / 实时控制     │
        │  Timer / IRQ / 外设      │
        │                         │
        └─────────────────────────┘
```

这种设计能够将复杂 Linux 业务与实时控制任务进行 CPU、内存及外设资源隔离，降低两个系统之间的相互干扰。项目最终目标是实现 Linux 与 FreeRTOS 在不同 CPU 核心上的独立运行以及双向通信。

---

# 二、项目目标

本项目主要完成以下内容：

1. 理解并实现 RK3588 AMP 异构多核运行架构。
2. 保持 Linux SMP，多核继续运行 Linux。
3. 将指定 Cortex-A55 核从 Linux CPU 拓扑中隔离。
4. 在独立 Cortex-A55 核上运行 AArch64 FreeRTOS。
5. 完成 FreeRTOS ARM64 BSP 适配。
6. 实现 GICv3 中断控制。
7. 使用 ARM Generic Timer 实现 FreeRTOS Tick。
8. 实现 UART 调试输出。
9. 划分 Linux / FreeRTOS 独立内存。
10. 建立 Linux 与 FreeRTOS 共享内存通信。
11. 在共享内存基础上进一步验证 Mailbox / RPMsg。
12. FreeRTOS 完成传感器采集与实时控制。
13. Linux 完成数据显示、用户交互和控制指令发送。
14. 测试双系统通信延迟与长时间运行稳定性。

最终形成：

```text
Linux 复杂业务
        +
FreeRTOS 实时控制
        +
CPU / Memory 隔离
        +
Shared Memory / RPMsg IPC
```

的 RK3588 异构双系统控制架构。

---

# 三、系统总体架构

最终目标系统为：

```text
RK3588 8 Core
│
├── Debian Linux
│   │
│   ├── Cortex-A76
│   ├── Cortex-A76
│   ├── Cortex-A76
│   ├── Cortex-A76
│   ├── Cortex-A55
│   ├── Cortex-A55
│   └── Cortex-A55
│
│       Linux SMP
│
│       ├── 网络通信
│       ├── 数据显示
│       ├── 用户交互
│       ├── 日志记录
│       └── 控制命令发送
│
└── FreeRTOS
    │
    └── Cortex-A55 cpu_l3
        MPIDR = 0x300

        ├── 实时传感器采集
        ├── 风扇 / 执行器控制
        ├── GICv3 IRQ
        ├── Generic Timer
        └── 实时任务调度
```

Linux 与 FreeRTOS 不依赖同一个操作系统调度器，两侧分别独立运行，通过共享内存、Mailbox 或 RPMsg 完成通信。

---

# 四、AMP 启动架构

项目使用 Rockchip SDK 提供的 AMP Framework，而不是简单通过手工启动从核。

FreeRTOS 固件经过编译后生成：

```text
freertos.elf
        ↓
freertos.bin
        ↓
amp.its
        ↓
amp.img
```

U-Boot 加载 `amp.img` 后，根据 AMP FIT Image 中描述的 CPU、Load Address、Entry Address 等信息启动 FreeRTOS。

目标启动链路：

```text
U-Boot
  ↓
amp.img
  ↓
Rockchip AMP Framework
  ↓
PSCI CPU_ON
  ↓
SMC
  ↓
BL31 / Trusted Firmware
  ↓
Cortex-A55 cpu_l3
  ↓
FreeRTOS
```

RK3588 AMP FIT Image 第一版规划：

```text
arch = arm64

cpu = 0x300

load = 0x08400000

boot-on = 1
```

其中 `cpu = 0x300` 对应计划运行 FreeRTOS 的 `cpu_l3`。

---

# 五、CPU 核心隔离

项目不会关闭 Linux SMP。

Linux 仍然运行于其余 CPU 核心，仅将一个 Cortex-A55 核从 Linux CPU 拓扑中移除。

目标核心：

```text
cpu_l3

Cortex-A55

MPIDR = 0x300
```

设备树隔离思路：

```text
Linux CPU Topology

cpu0
cpu1
cpu2
cpu3
cpu4
cpu5
cpu6
cpu7
 │
 └── remove cpu_l3
```

最终形成：

```text
Linux
7 Core
SMP

+

FreeRTOS
1 Core
AMP
```

这与“关闭 Linux SMP，让 Linux 只运行在单核”的方式不同。本项目保留 Linux 的多核处理能力，只将实时任务需要的 CPU 独立出来。

---

# 六、内存规划

AMP 架构不仅需要 CPU 隔离，还必须进行物理内存隔离。

如果 Linux 与 FreeRTOS 同时访问同一片普通内存，可能出现：

```text
内存覆盖
Cache 不一致
数据破坏
随机崩溃
```

因此项目通过 Device Tree `reserved-memory` 与 FreeRTOS Linker Script 进行内存区域划分。

当前第一版规划：

```text
AMP Shared Memory

0x07800000
Size: 4 MB
```

```text
RPMsg

0x07C00000
Size: 4 MB
```

```text
RPMsg DMA

0x08000000
Size: 1 MB
```

```text
FreeRTOS

0x08400000
~
0x093FFFFF

Size: 16 MB
```

FreeRTOS 第一版：

```text
Link Address  = 0x08400000
Load Address  = 0x08400000
Entry Address = 0x08400000
```

Linux 不使用 FreeRTOS 独占区域，从而防止两个系统发生内存冲突。

---

# 七、FreeRTOS BSP

项目使用：

```text
FreeRTOS Kernel V10.5.1
```

ARM64 移植基础：

```text
portable/GCC/ARM_CA53_64_BIT_SRE
```

该 Port 使用 AArch64，并采用 GICv3 System Register Interface，作为 RK3588 Cortex-A55 FreeRTOS 移植基础。

FreeRTOS BSP 主要包括：

```text
startup.S
linker.ld

FreeRTOSConfig.h

uart.c
uart.h

gic.c
gic.h

timer.c
timer.h

main.c
```

各部分职责：

`startup.S`

负责 CPU 从 AMP 启动入口进入 FreeRTOS 前的底层初始化，包括：

```text
Stack
BSS
异常向量
EL1运行环境
C Runtime
```

`linker.ld`

负责定义：

```text
FreeRTOS固件链接地址
代码段
数据段
BSS
Stack
Heap
```

`gic.c`

负责 RK3588 GICv3 中断控制，包括 Distributor、Redistributor 和 CPU Interface。

`timer.c`

使用 ARM Generic Timer 为 FreeRTOS Scheduler 提供系统 Tick。

`uart.c`

提供 FreeRTOS 独立运行阶段的调试输出。

`FreeRTOSConfig.h`

负责调度器、任务优先级、Tick、内存以及 Hook 等 FreeRTOS 参数配置。

---

# 八、FreeRTOS 任务设计

FreeRTOS 后续主要承担实时任务，例如：

```text
Sensor Task
    ↓
AHT10
    ↓
温湿度采集
    ↓
Shared Memory
```

以及：

```text
Linux Command
      ↓
Shared Memory / RPMsg
      ↓
FreeRTOS Control Task
      ↓
Fan / GPIO / PWM
```

第一版计划包含：

```text
sensor_task
control_task
communication_task
```

传感器数据可采用：

```text
100 ms
```

周期采集。

但是控制命令处理不能简单放在同一个 100 ms 轮询周期中。

项目的两个指标需要分开设计：

```text
传感器采样周期

100 ms
```

和：

```text
Linux → FreeRTOS
控制命令响应时间

≤ 20 ms
```

因此控制命令需要独立处理，通过更高频检查、通知机制或独立实时任务避免最坏情况下等待完整的 100 ms 采样周期。

---

# 九、Linux / FreeRTOS 通信

第一阶段通信优先使用：

```text
Shared Memory
```

先完成最小可用的双向通信链路。

后续逐步升级：

```text
Shared Memory
      +
Mailbox
      +
RPMsg
```

最终形成完整 IPC。

通信方向包括：

```text
FreeRTOS
   ↓
Temperature
Humidity
Fan Status
   ↓
Linux
```

以及：

```text
Linux
   ↓
Control Command
   ↓
FreeRTOS
   ↓
Fan / GPIO / PWM
```

共享内存两侧需要使用统一的数据结构，并保证：

```text
数据类型一致
结构体大小一致
内存对齐一致
字节序一致
Cache一致性
Memory Barrier正确
```

项目规划先使用共享内存验证数据链路，再逐步加入 Mailbox 与 RPMsg 通知机制。

---

# 十、业务功能

完成底层 AMP、FreeRTOS BSP 和 IPC 后，再接入实际业务。

第一版业务：

```text
AHT10
 │
 ▼
FreeRTOS Sensor Task
 │
 ├── Temperature
 └── Humidity
 │
 ▼
Shared Memory / RPMsg
 │
 ▼
Linux
 │
 └── 数据显示
```

反向控制：

```text
User

fan high
   │
   ▼
Linux Command Parser
   │
   ▼
Shared Memory / RPMsg
   │
   ▼
FreeRTOS Control Task
   │
   ▼
Fan
```

Linux 负责：

```text
数据显示
用户交互
网络通信
日志存储
命令下发
```

FreeRTOS 负责：

```text
实时采集
实时控制
IRQ处理
低延迟命令执行
```

---

# 十一、项目目录

当前工程目录：

```text
RK3588_AMP_Controller/
│
├── board/
│   ├── dts/
│   └── uboot/
│
├── common/
│   ├── amp_shared.h
│   └── amp_protocol.h
│
├── linux/
│   ├── main.cpp
│   ├── shared_memory.cpp
│   └── command.cpp
│
├── rtos/
│   ├── startup.S
│   ├── linker.ld
│   ├── FreeRTOSConfig.h
│   ├── main.c
│   ├── uart.c
│   ├── uart.h
│   ├── gic.c
│   ├── gic.h
│   ├── timer.c
│   ├── timer.h
│   ├── shared_memory.c
│   ├── shared_memory.h
│   ├── sensor_task.c
│   ├── sensor_task.h
│   ├── control_task.c
│   └── control_task.h
│
├── scripts/
│   ├── build_linux.sh
│   ├── build_rtos.sh
│   └── deploy.sh
│
├── tests/
│   ├── latency_test.cpp
│   └── shared_memory_test.cpp
│
├── third_party/
│   └── FreeRTOS-Kernel/
│
├── Makefile
└── README.md
```

---

# 十二、当前开发环境

开发板：

```text
ATK-DLRK3588B
```

SoC：

```text
Rockchip RK3588
```

CPU：

```text
4 × Cortex-A55
4 × Cortex-A76
8 Core ARM64
```

当前 Linux：

```text
Debian Linux
```

SDK：

```text
~/rk3588_linux_sdk
```

项目：

```text
~/rk3588_projects/RK3588_AMP_Controller
```

FreeRTOS：

```text
~/rk3588_projects/RK3588_AMP_Controller/
third_party/FreeRTOS-Kernel
```

本项目与：

```text
RK3588_AI_Vision
```

为两个独立项目，代码和配置不混用。

---

# 十三、当前项目状态

目前开发板仍运行普通 Linux SMP。

```text
cat /sys/devices/system/cpu/online

0-7
```

说明 RK3588 当前 8 个 CPU 核心仍全部由 Linux 管理。

当前状态：

```text
Linux     ：8 Core
FreeRTOS  ：0 Core
```

目标状态：

```text
Linux     ：7 Core
FreeRTOS  ：1 Core
```

因此当前项目还不能描述为已经完成 Linux + FreeRTOS AMP 双系统。

已经确认的基础条件包括：

```text
RK3588 AMP Framework      已存在
AMP Device Tree           已存在
PSCI v1.1                 已确认
SMC Calling Convention    已确认
GICv3                     已确认
ARM64 FreeRTOS Port       已确认
cpu_l3 / MPIDR 0x300      已确认
FreeRTOS候选内存          已确认
UART5资源                 已确认
```

尚未完成：

```text
cpu_l3正式隔离
FreeRTOS目标核实际启动
FreeRTOS Scheduler运行
FreeRTOS Tick运行
共享内存双向通信
Mailbox通信
RPMsg通信
AHT10实际采集
风扇实际控制
≤20 ms延迟验证
30 min稳定性验证
```

---

# 十四、开发计划

整个项目严格按照以下原则推进：

> **先证明 Cortex-A55 能独立执行代码，再运行 FreeRTOS，最后实现通信和实际业务。**

不在 AMP Boot 尚未验证的情况下直接调试 FreeRTOS Scheduler，也不在 FreeRTOS 尚未稳定运行的情况下提前加入复杂通信和业务。

| 阶段                    | 先检查／修改哪些文件                                                                                | 达到什么目标                                         | 设计原因                                |
| --------------------- | ----------------------------------------------------------------------------------------- | ---------------------------------------------- | ----------------------------------- |
| 1. 确认实际启动配置           | SDK 的 `BoardConfig*.mk`、实际板级 DTS、U-Boot `.config`                                         | 找准正在使用的设备树、AMP 配置与镜像加载方式                       | 避免修改了示例配置，但实际开发板根本没有使用              |
| 2. 隔离资源               | 实际板级 DTS，项目 `board/dts/` 保存对应修改                                                           | 从 Linux 移除 `cpu_l3`，确认并保留 RTOS 内存              | 防止两个系统同时使用同一 CPU 或物理内存              |
| 3. 打包裸机镜像             | 新增 `board/uboot/amp.its`，调整 `Makefile`；按照实际 SDK 配置启用 U-Boot AMP                           | 将现有 `amp_bootstrap.bin` 打包为 `amp.img`，明确实际加载路径 | 建立 U-Boot → AMP → PSCI → A55 的完整启动链 |
| 4. 验证目标核启动            | 检查 `startup.S`、`linker.ld`、`rtos/main.c`，必要时修改                                            | 在目标核正确读回 Magic、EL、MPIDR 和 Stage，同时 Linux 正常运行  | 先排除镜像、地址、CPU 和启动链问题                 |
| 5. 完成 FreeRTOS BSP    | 修改 `startup.S`、`FreeRTOSConfig.h`、`main.c`、`Makefile`；实现 UART、GIC、Timer                   | 两个简单任务能够稳定切换，Delay 与 Tick 正常                   | 先验证 RTOS 调度器可靠性，再接入业务               |
| 6. 实现共享内存双向通信         | `common/amp_shared.h`、`amp_protocol.h`、Linux / RTOS 两侧 `shared_memory.*`、`linux/main.cpp` | Linux 发命令，RTOS 执行并回复，并建立明确的 Cache 与同步规则        | 使用最小协议首先验证 Linux ↔ RTOS 双向数据链路      |
| 7. 加入 Mailbox / RPMsg | 板级 DTS、RTOS 通信模块、Linux 接口                                                                 | 完成通知机制和 RPMsg 双向验证                             | 在共享内存已经稳定的基础上逐层增加 IPC 复杂度           |
| 8. 接入实际业务             | `sensor_task.c`、`control_task.c`、Linux `command.cpp`，补充对应外设驱动                             | 完成 AHT10 采集、风扇控制以及 Linux 数据显示                  | 此时 Boot、Scheduler 与 IPC 已经稳定        |
| 9. 验收记录               | `tests/`、`docs/`                                                                          | 测量命令响应 ≤20 ms，并连续运行至少 30 min                   | 使用实际实验结果证明实时性和稳定性                   |

---

# 十五、当前执行阶段

当前优先推进：

```text
Task 1

确认实际启动配置
```

现阶段：

```text
暂不修改 AMP 核心代码
```

首先需要确认三个关键信息：

```text
1. 实际 RK_KERNEL_DTS

2. U-Boot AMP Configuration

3. amp.img 实际加载路径
```

需要重点检查：

```text
RK3588 Linux SDK

BoardConfig*.mk

U-Boot .config

实际 Board DTS

AMP DTS

启动脚本 / FIT Image配置
```

原因是 Rockchip SDK 中通常存在多个开发板配置、设备树以及 AMP 示例。

只有确定开发板真正使用：

```text
哪个 BoardConfig

哪个 DTS

哪个 U-Boot配置

哪个 AMP Image
```

之后，才能准确修改目标文件。

当前已有裸机代码：

```text
amp_bootstrap.bin
```

继续保留，作为第一轮上板测试程序。

第一阶段的目标不是运行 FreeRTOS，而是证明：

```text
Linux正常运行

+

cpu_l3独立启动

+

cpu_l3能够执行自己的AArch64代码
```

验证信息包括：

```text
Magic
EL
MPIDR
Stage
```

理想验证结果：

```text
Linux
正常启动

cpu_l3
独立执行代码

MPIDR
0x300

EL
符合预期

Memory
无冲突
```

完成这一阶段以后，再开始 FreeRTOS Scheduler 移植。

---

# 十六、测试与验收

## 1. AMP Boot

验证：

```text
Linux正常启动
FreeRTOS目标CPU正常启动
目标CPU MPIDR正确
Linux不再调度cpu_l3
```

## 2. FreeRTOS Scheduler

验证：

```text
Task A
Task B
```

可以正常进行：

```text
Create
Switch
Delay
Wakeup
```

## 3. Timer

验证：

```text
FreeRTOS Tick稳定
vTaskDelay正常
系统运行时间正常
```

## 4. IPC

验证：

```text
Linux → FreeRTOS

FreeRTOS → Linux
```

双向数据：

```text
无明显丢失
无数据错乱
无重复处理
无死锁
```

## 5. 实时性能

重点测试：

```text
Linux发送命令
        ↓
FreeRTOS收到命令
        ↓
执行控制
```

目标：

```text
Response Latency ≤ 20 ms
```

这里的：

```text
100 ms Sensor Sampling
```

和：

```text
≤ 20 ms Command Response
```

属于两个不同实时指标，必须采用不同任务或通知机制设计。

## 6. 稳定性

第一阶段：

```text
连续运行 ≥ 30 min
```

后续继续进行：

```text
1 h
8 h
24 h
```

长时间稳定性测试。

验收阶段需要同时观察 FreeRTOS 任务、Linux 应用以及 IPC 是否稳定，并记录通信延迟、数据完整性和异常状态。项目原始任务要求中也明确提出控制响应延迟目标为 `≤20 ms`，并至少进行 `30 min` 连续运行测试。

---

# 十七、关键技术点

本项目涉及的主要嵌入式技术包括：

```text
RK3588
ARMv8-A / AArch64
Cortex-A55
Cortex-A76

AMP
Linux SMP
FreeRTOS

U-Boot
FIT Image
PSCI
SMC
Trusted Firmware / BL31

Device Tree
CPU Topology
reserved-memory

GICv3
ICC_*_EL1

ARM Generic Timer

EL1
VBAR_EL1
SPSR_EL1
ELR_EL1

MMU / Cache
Memory Barrier

Shared Memory
Mailbox
RPMsg

UART
I2C
GPIO
PWM

Linux / FreeRTOS IPC
```

---

# 十八、项目最终成果

项目完成后预计形成：

* RK3588 Debian Linux + FreeRTOS AMP 双系统运行环境。
* Linux SMP + FreeRTOS 单核异构 CPU 架构。
* Cortex-A55 CPU 独立启动机制。
* FreeRTOS ARM64 BSP。
* GICv3 中断系统。
* ARM Generic Timer FreeRTOS Tick。
* UART 调试模块。
* CPU / Memory / Peripheral 资源隔离方案。
* Rockchip AMP FIT Image。
* Linux / FreeRTOS Shared Memory IPC。
* Mailbox / RPMsg 双向通信。
* AHT10 温湿度实时采集。
* FreeRTOS 实时控制任务。
* Linux 数据显示与控制命令接口。
* IPC 延迟测试。
* 长时间稳定性测试。
* RK3588 AMP 系统启动、调试及异常排查文档。

最终形成完整的数据闭环：

```text
            Linux
              │
        Control Command
              │
              ▼
      Shared Memory / RPMsg
              │
              ▼
          FreeRTOS
              │
       Real-Time Control
              │
              ▼
        Sensor / Fan
              │
        Sensor Status
              │
              ▼
      Shared Memory / RPMsg
              │
              ▼
            Linux
              │
      Display / Network
```

---

# 十九、项目定位

本项目重点不是简单地在 RK3588 上移植一个 FreeRTOS Demo，而是围绕：

```text
Multi-Core SoC
        +
Linux / RTOS
        +
CPU Isolation
        +
Memory Isolation
        +
Real-Time Scheduling
        +
Inter-Core Communication
```

建立一个完整的异构多核控制系统。

项目可以进一步扩展到：

* 车载域控制器。
* 工业实时控制器。
* 机器人控制器。
* 无人设备控制平台。
* 智能终端。
* 边缘控制设备。

其核心架构与“通用 Linux 系统负责复杂业务，实时系统负责确定性控制”的域控制器设计思路一致。原始方案同样将 Linux/Android 一侧定位为交互、显示、网络等复杂业务，将实时系统定位为控制、采集和总线业务，并通过共享内存/RPMsg进行系统间通信。
