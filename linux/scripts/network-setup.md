# 开机配置开发板网络

`linux/S97amp-network` 适用于当前 Buildroot `/etc/init.d/S*` 启动方式。
默认开机设置 eth0 为 192.168.88.101/24；板卡和虚拟机在同一子网时不需要网关。
不依赖 userdata 挂载，也不需要等待网线连接。

## 当前开发板安装

Ubuntu 项目目录执行：

```sh
scp linux/S97amp-network root@192.168.88.101:/userdata/amp/S97amp-network
```

开发板串口执行：

```sh
cp /userdata/amp/S97amp-network /etc/init.d/S97amp-network
chmod 755 /etc/init.d/S97amp-network
/etc/init.d/S97amp-network start
sync
```

重复执行 start/restart 可以重新应用配置。配置文件可选；需要改地址时，
把 `linux/scripts/network.conf.example` 安装为 `/etc/amp/network.conf` 并修改。
配置文件由 shell 读取，应由 root 管理。

## 随后续 rootfs 发布

Ubuntu 项目目录执行：

```sh
overlay="$HOME/rk3588_linux_sdk/buildroot/board/rockchip/rk3588/fs-overlay"
install -d "$overlay/etc/init.d"
install -m 755 linux/S97amp-network "$overlay/etc/init.d/S97amp-network"
```

随后重新构建 rootfs，把新 rootfs 纳入签名发布包。仅复制到 overlay，或者复用旧
rootfs.img，都不会让这个文件进入已生成的镜像。A/B 两槽需要分别通过新 rootfs
更新；手动安装只影响当前槽。

如果其他 DHCP 客户端或网络管理服务也配置 eth0，应统一设置，避免它们随后修改
静态地址或路由。现有 S98amp-access 若仍设置相同地址，会重复应用同一配置。

本脚本只配置网络；SSH 公钥和主机密钥的持久化是独立配置。
