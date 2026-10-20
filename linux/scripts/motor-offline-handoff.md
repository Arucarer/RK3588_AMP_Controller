# MotorSIM 离线交付准备

暂时不能上机时，用以下命令构建并保存配套产物：

```sh
python3 tools/prepare_motor_sim.py --output build/motor-sim-offline-v1
```

输出目录必须不存在，重复执行请换目录名。默认SDK为~/rk3588_linux_sdk，
可通过--sdk指定；--jobs默认4，范围1..32。

工具把当前工作区相关源码复制到临时目录后构建，不执行make clean，不覆盖
现有build/amp.img，不修改Git、SDK或开发板。未提交的源码修改会参与构建，
sources.json记录实际快照文件摘要，manifest.json记录HEAD及工作区状态。

输出包含：firmware/amp.img及对应ELF/BIN/MAP、linux/amp_motorctl、编译日志、
FIT内容列表、源码摘要、产物清单和SHA256SUMS。保留ELF/MAP便于后续定位，
不能拿其他版本符号地址解释这份固件。

```sh
cd build/motor-sim-offline-v1
sha256sum -c SHA256SUMS
```

build-state.json为OFFLINE_BUILT只代表本地主机完成编译与产物检查。
board_verified固定false；它不是签名OTA包，不包含boot.img/rootfs.img。
SHA256校验不提供身份认证。构建失败时目录会保留FAILED状态和日志，不自动删除。

后续具备硬件条件时，需要沿用既有协调签名发布流程，选择匹配的Linux boot和
rootfs，并完成新RTOS试启动、健康确认、LED/总线回归、模拟电机业务和HMI/
看门狗并发测试。当前不提供直接dd、刷分区或自动重启指令。

本包只有SIM状态机与模拟模型，默认禁用，不包含FOC、PWM/ADC驱动或真实电机
控制验收。离线准备不会把待验收项变成已验收项。
