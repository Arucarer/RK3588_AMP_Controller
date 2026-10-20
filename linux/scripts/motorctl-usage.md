# MotorSIM Linux 命令行

工具只接受SIM=1响应，不能驱动真实电机。需要配套的新RTOS（包含motor IPC），
现有已部署旧固件可能返回UNSUPPORTED；本阶段不部署或刷写。

Ubuntu项目目录构建：

```sh
sh tools/build_motorctl.sh
```

输出build/linux/amp_motorctl，为ARM64静态程序，放到开发板后执行。
后续统一部署再使用下列示例，现在不必运行：

```sh
/userdata/amp/bin/amp_motorctl status
/userdata/amp/bin/amp_motorctl run 500 5
/userdata/amp/bin/amp_motorctl run -500 5
```

run默认完成使能、目标设置、启动、每200ms续租、减速停止及禁用。
停止最多等待5秒，失败或Ctrl-C时尽力禁用并以非零码退出。不自动复位故障、
不重发结果未知的目标或启动命令。通信失败无法证明设备已经停止，RTOS租约
是SIM后备保护，真实硬件仍需要独立故障关断。

独立命令：enable、target RPM、start、stop、heartbeat、disable、reset、
inject LOAD FLAGS。转速范围-3000..3000，run时长1..3600秒；注入负载0..1000，
FLAGS按位或：1堵转、2反馈丢失、4合成过温。inject 0 0清除注入，不能清除锁存
故障。独立start不会自动续租；若1秒内没有显式heartbeat，控制器将锁存租约故障。
终端逐条敲命令容易超时，正常演示优先使用run。

默认令牌文件/run/amp-motor.token，按需生成非零64位令牌，0600、当前用户所有。
它是协作控制权标识，不是认证密码，不打印令牌。跨命令应使用同一路径；可用
`--token-file PATH`指定。不要在电机运行时删除、更换令牌文件或使用不同令牌
抢占运行中的控制器。重启后/run通常清空，RTOS也应回到禁用状态。

命令执行期间锁住令牌文件，避免另一个修改进程争用；status不读取令牌文件。
每笔交易重新打开/关闭IPC，睡眠期间不持有IPC锁，允许健康服务和HMI查询。
IPC锁忙时仅重试获取锁，最多约200ms；不能据此承诺请求一定成功或板端无争用。

主机测试：`python3 tests/motor_cli_test.py`。测试使用真实桥接、状态机、SIM和
消息编解码，但IPC映射、任务队列和时钟是替身，不是共享内存/RTOS板端验收。
