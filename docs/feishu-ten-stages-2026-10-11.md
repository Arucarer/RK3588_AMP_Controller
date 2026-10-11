# RK3588 Linux + FreeRTOS AMP：十阶段进度与 A/B OTA 验收记录

记录日期：2026-10-11。范围以现有源码、主机测试输出和开发板串口记录为准。已编译、模拟通过、板级验证通过分别记录，不相互替代。

## 一、项目定位与当前结论

项目基于 RK3588 构建 Linux + FreeRTOS AMP 平台。FreeRTOS 使用独立 Cortex-A55 核，Linux 使用其余七核；双方通过共享内存协议完成命令、状态和心跳交互。板载 LED 已形成 Linux → IPC → FreeRTOS ControlTask → GPIO → 状态返回的控制链路。

本轮完成阶段十的 A/B 更新基本闭环：认证发布包、写入非活动槽、写后校验、提交 pending、U-Boot 统一选槽、试启动扣次、健康检查自动确认、失败回退和硬件看门狗接管。

最终 A/B 两槽均部署 amp-demo-0.2.1；当前 A 槽已自动确认，元数据 generation=24。不能据此宣称原始车载域控制器任务书全部完成。

## 二、十阶段进度

| 阶段 | 内容 | 当前实际状态 |
|---|---|---|
| 1 | AMP CPU、内存规划与启动 | 已完成平台基本启动。FreeRTOS MPIDR=0x300，Linux 七核，预留 0x20000000–0x20ffffff 共 16 MiB。CPU/内存划分不等于完整硬件安全隔离。 |
| 2 | EL1 异常、GICv3、Generic Timer | 异常记录、定时器中断已验证，FreeRTOS Tick 为 100 Hz。 |
| 3 | FreeRTOS 调度和最小共享内存通信 | 多任务计数持续推进，Linux 与 FreeRTOS 加法 request-response 已验证。 |
| 4 | IPC 协议工程化 | V2 协议、序号、会话、CRC、PING、STATUS、HEARTBEAT、客户端重连已实现；正常通信和重连已上板。Mailbox、RPMsg 尚未完成。 |
| 5 | FreeRTOS 实时控制域 | ControlTask、IpcTask、MonitorTask 与板载 LED 控制、闪烁、状态反馈已验证。PWM、GPIO IRQ 实测延迟及完整多外设任务架构尚未完成。 |
| 6 | CAN/CAN FD | 已调查 CAN1 地址、中断、时钟、驱动和回环支持。没有完整 FreeRTOS 独占 CAN 收发、Bus-Off 恢复及板级总线链路验收证据。 |
| 7 | RS485/Modbus 与外设资源划分 | 未形成 RS485/Modbus 实际收发闭环。LED GPIO 资源划分已落实，不代表全部外设隔离完成。 |
| 8 | Linux HMI、数据管理 | Web HMI、SSH 隧道访问、SQLite 操作记录及服务重启后继续记录已验证。Android、多媒体、4G 未作为本项目完成项。 |
| 9 | 实时性和故障隔离 | IPC 当前负载基线与七个 CPU 运算进程压力场景各 1000 次请求全部成功。未完成 Tick jitter、GPIO IRQ、CAN 时延、Linux 整机重启隔离等完整验收。 |
| 10 | A/B OTA 与可靠性 | 已完成签名发布、离线布局迁移、双槽部署、切槽与确认、三次启动准备失败回退、停止喂狗自动复位。写入进程中断测试的重启结果正常，但当前保存证据缺少注入 PASS；实际掉电恢复未验收。 |

进度不是线性推进：阶段十基本闭环已完成，但阶段六、七仍缺主要功能，阶段四、五、九也有未完成项。不宜按“做到第十阶段”推导为 100% 完成。

## 三、阶段五至九的已验证结果

### 板载 LED 实时控制

Linux ampctl 支持 led on、off、blink 和 status。FreeRTOS 执行控制并反馈状态，PING 可以继续工作。实体 LED 亮灭与闪烁效果已观察确认。Linux 设备树禁用 work LED 节点，为 GPIO1_A3 设置资源预留，GPIO bank 保持启用。

### 健康监测

MonitorTask 输出 ready、faults、latched、checks 和任务 age 信息。最终版本 ready=1，faults=0，latched=0，Tick 与 checks 持续推进。旧固件的 HEALTH unavailable 不能作为 MonitorTask 已部署的证据。

### HMI 与 SQLite

HMI 监听 127.0.0.1:8088，经 SSH 隧道由 Ubuntu 浏览器访问。状态、PING、LED 命令和操作历史形成应用闭环。SQLite quick_check 输出 ok；服务重启后新增记录 id=12，旧记录保留。

client_total_ms 包含进程启动、HELLO 和请求响应，不能作为纯 IPC 延迟。

### IPC 性能基线

| 场景 | 成功/请求 | 平均 RTT | P99 | 采样最大值 |
|---|---|---|---|---|
| 当前后台负载基线 | 1000/1000 | 19.996 ms | 21.009 ms | 21.202 ms |
| 增加七个 CPU 运算进程 | 1000/1000 | 20.003 ms | 20.062 ms | 20.063 ms |

这是特定负载下的请求往返测量。不能推导所有 Linux 核持续满载，也不能将采样最大值解释为硬实时最坏情况保证。

## 四、阶段十：实现与部署

### 发布和启动机制

发布包包含 boot.img、rootfs.img、amp.img、release.json 和 release.sig。通过包外可信公钥执行 RSA/SHA256 验签，同时检查镜像哈希、DTB CPU/预留内存、FIT 与 resource 的 DTB 一致性以及 rootfs 应用文件。

U-Boot 从同一启动上下文选择 DTB/resource、Kernel、AMP 与 rootfs PARTUUID。试启动先持久化扣次，再加载候选槽；禁止失败后混用另一槽镜像。

abmeta 使用固定小端记录、CRC 和双副本选择。写入新代次时保留旧副本，刷新后读回校验。双副本不等于 eMMC 原子写入或独立故障域。

Linux 更新器只写非活动槽；三个镜像写后校验全部通过，才提交 pending。Linux 确认工具核对启动身份、实际根分区、PARTUUID、代次并锁定元数据访问。

包验签不等于完整安全启动链；FIT SHA256 本身不提供签名认证。

### 离线迁移

迁移 FIT 从 FAT32 U 盘加载到 RAM，根文件系统为 rootfs；迁移前 eMMC 分区未挂载，swap 和 holders 为空。五个迁移 Python 模块与布局 JSON 已打包。

GPT 主备副本与原布局预检通过，备份转存到 Windows。userdata 文件系统从 46,447,702,016 字节缩小为 31,242,321,920 字节，缩容后 e2fsck 通过。随后提交新 GPT，读回及内核分区核对通过。

| 分区 | 起始扇区 | 扇区数 | 作用 |
|---|---:|---:|---|
| boot / p3 | 32768 | 131072 | A 槽 Linux FIT |
| amp / p5 | 425984 | 65536 | A 槽 FreeRTOS FIT |
| rootfs / p6 | 491520 | 29360128 | A 槽根文件系统 |
| userdata / p8 | 30113792 | 61151232 | 两槽共享持久数据 |
| boot_b / p9 | 91265024 | 131072 | B 槽 Linux FIT |
| amp_b / p10 | 91396096 | 65536 | B 槽 FreeRTOS FIT |
| rootfs_b / p11 | 91461632 | 29360128 | B 槽根文件系统 |
| abmeta / p12 | 120821760 | 8192 | 启动元数据 |

扇区大小 512 字节。原八个 PARTUUID 保留；新增 B 槽 rootfs PARTUUID 为 89cbbad9-db63-438d-a7cf-4333871cf53b，A 槽为 614e0000-0000-4b53-8000-1d28000054a9。

### 板级验收证据

1. 初始 confirmed=A、generation=1 启动正常，FreeRTOS 与 HMI 健康。
2. 看门狗初始化检查误判导致候选启动准备连续失败三次，系统回退 A 槽；证实扣次及该失败路径的回退。
3. 修复 U-Boot 后 B 槽进入 Linux；RAM 临时修正健康服务后，B 槽自动确认至 generation=15。
4. 永久修复纳入 0.2.1，A 槽自动确认至 generation=18。
5. B 槽更新 0.2.1 并自动确认至 generation=21；看门狗 active。
6. SIGSTOP 暂停健康进程后，设备自动重新进入 DDR/SPL/U-Boot，仍启动已确认 B 槽 generation=21。没有精确计时或独立复位原因寄存器记录，不报告精确复位延迟。
7. 更新中断测试后仍启动 B 槽 generation=21，FreeRTOS 健康；当前保存输出未包含注入脚本 PASS，不能扩大为完整中断/掉电验收。
8. A 槽完整恢复并自动确认至 generation=24，ALREADY_CONFIRMED，HMI 持续 200，看门狗由正式健康服务接管。

## 五、关键问题与解决过程

| 问题 | 定位证据 | 处理与结果 |
|---|---|---|
| U-Boot 无法截住启动 | bootdelay=0，只检查一次 Ctrl+C；Linux/PowerShell 中执行 U-Boot 命令无效 | 独占 COM5，连续发送纯 0x03，成功进入 =>。 |
| COM5 访问被拒绝 | 串口工具同时占用端口 | 关闭其他串口会话后使用，避免多个程序争用。 |
| recovery.img 不存在 | output/firmware 软链接存在但目标缺失 | 构建独立迁移 ramdisk，使用正确 CHIP_DIR 等 SDK 环境打包 FIT。 |
| source SDK 配置报未绑定变量 | 脚本 set -u 与可选配置变量冲突 | source 时暂时关闭 nounset，随后恢复。 |
| FIT Invalid header | U盘实际文件名与 fatload 参数不一致，文件未加载成功 | 使用真实文件名；成功读取后才 boot_fit。 |
| 以为启动仍未完成 | 驱动后台日志覆盖 Shell 提示符 | 已出现 AMP offline migration shell 即进入 RAM Shell，按回车继续。 |
| 备份误写到 RAM 挂载点 | mount 失败后复制仍继续，假 PASS | 确认实际挂载后复制，并保存到 Windows 硬盘；挂载失败不能继续。 |
| openssl/debugfs 配置已开但镜像缺文件 | Buildroot 增量安装状态未更新；早期 debugfs 检查命令也有误用 | 重配相关包并重建，最终从实际 rootfs 提取确认 AArch64 工具存在。 |
| TPL 编译缺少 disk_partition.uuid | A/B 模块错误进入 TPL | Makefile 将 A/B 模块限制在完整 U-Boot，SDK 正式打包成功。 |
| 看门狗计数误判 | TORR=0x0f；CCVR 从 0x7ffffffe 递减至 0x7ffff697 | 不再强制 TOPINIT 高四位读回 0xf，只检查 TOP 低四位、复位模式与计数推进。 |
| Linux 拒绝继承看门狗 | state=inactive，但 CR=0x09、timeleft=54、CCVR 非零 | 区分软件 active 与 WDOG_HW_RUNNING，以硬件模式及剩余计数核对继承条件，再打开设备接管。 |
| 健康服务失败后 IPC 超时 | 服务在启动握手之前因看门狗检查退出 | 修正继承检查后，握手、MonitorTask、HMI 和自动确认均恢复。 |
| SSH 主机密钥变化 | 新 rootfs 生成新主机密钥 | 通过 COM5 串口核对指纹后更新 Ubuntu known_hosts。 |
| SSH 密码失败 | 使用旧 rootfs 密码 | 串口设置新密码；宿主机与开发板命令需按提示符区分。 |

## 六、验收边界与后续安排

本轮 A/B 部署与基本验收结束，不继续追加测试。应保存串口记录、发布包、公钥指纹、修复版 U-Boot 及 Git 提交。当前 U-Boot 为单份公共启动器，不是双槽 U-Boot OTA。

实际掉电中断、元数据提交瞬间故障、全启动阶段卡死、长期可靠性和防回滚版本策略不在本轮已验收范围。已确认槽正常启动时当前实现 wdt=0，不保证每次正常启动都有持续运行的健康看门狗。

回到原任务书，下一阶段应补齐 CAN/CAN FD 和 RS485/Modbus 的真实硬件链路、PWM/GPIO IRQ 实测与完整资源隔离。当前平台可以作为这些功能的基础，不能替代这些功能本身。
