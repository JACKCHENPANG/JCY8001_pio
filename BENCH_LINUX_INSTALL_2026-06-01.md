# 台架 Linux 机：从 Live USB 升级为持久 Ubuntu + GUI venv

**日期**: 2026-06-01
**机器**: JCY8001 EIS 台架 linux 测试机
**操作**: 远程无头 (headless, 仅 SSH over LAN) 完成

---

## 连法（关了重开直接用）

```bash
# mac 同网段 (192.168.0.54) 直达；无 key，密码登录
sshpass -p 12345678 ssh -o StrictHostKeyChecking=no \
  -o PreferredAuthentications=password -o PubkeyAuthentication=no \
  ubuntu@192.168.0.53
```

- 用户 `ubuntu` / 密码 `12345678`，**sudo 免密** (NOPASS)
- **静态 IP 192.168.0.53**（重启不变）
- ⚠️ 重装过系统，SSH host key 变了。连不上先在 mac 跑：`ssh-keygen -R 192.168.0.53`

---

## 做了什么（A + B）

### A — GUI 验证（离屏截图）
- 台架装 PyQt5+pyqtgraph，用 `QT_QPA_PLATFORM=offscreen` 跑 `upper_computer/main.py`，不需 X server
- 自主扫频 + 单点测量 两个 tab 都干净渲染，串口识别 `/dev/ttyS0`

### B — 持久系统（根问题解决）
- **原问题**: 系统跑 live USB（sdc1 挂 /cdrom）+ /cow 内存叠加层（只 ~500M）→ 装东西就满、重启全丢
- **解决**: debootstrap 无头装 **Ubuntu 22.04 到 sda（120G 三星 SSD）**，UEFI + grub，BootOrder ubuntu 第一
- 重启实测：`/` = `/dev/sda2`（不再是 /cow），101G 空闲，hostname `jcy-bench`
- 静态网络 (systemd-networkd)、ssh、Python3+venv+pip、清华 apt 镜像、Noto CJK 中文字体

### A 落到 B 上 — GUI venv 常驻持久系统
- 位置 `~/jcy8001/`（venv + upper_computer/main.py + shot.py + shot2.py + shot.sh）
- 离屏截图: `~/jcy8001/shot.sh [输出.png]`
- 实连设备跑界面: `~/jcy8001/venv/bin/python ~/jcy8001/upper_computer/main.py`（需显示器或 VNC）

---

## 硬盘布局（现状）

| 盘 | 分区 | 用途 |
|----|------|------|
| sda 120G 三星SSD | sda1 512M vfat=EFI / sda2 111G ext4 | **新系统根 `/`**（101G空闲）|
| sdb 500G 西数HDD | sdb7 79G ext4 / sdb5 193G NTFS(软件) / sdb6 193G NTFS(文档) | 数据盘 + 旧 venv(/jcywork) + sda 备份 |
| sdc 29G | sdc1 vfat UBUNTU 22_0 | **live USB，保留当退路**（装坏可 USB 启动）|

- sda 旧内容（半装的 Ubuntu 壳子，零用户数据，4G 是 swapfile）已备份：
  `/dev/sdb5(NTFS,软件) : sda2_backup_20260601.tar.gz` (35MB, 3695文件)
  > 新系统读 NTFS 需 `sudo apt install ntfs-3g` 再挂载

---

## 装机踩坑（重要，复用时注意）

1. **pypi 直连 timeout** → 全程用清华镜像 `-i https://pypi.tuna.tsinghua.edu.cn/simple`
2. **pip 临时文件爆 /cow**（live USB 时）→ `TMPDIR=/target/...` 改到大盘
3. **initrd 没生成** → `--no-install-recommends` 漏了 `initramfs-tools`，kernel 装了但 initramfs 没建。
   重启前必查 `/boot/initrd.img-*` **实体文件存在**（不只是 symlink），否则一重启变砖
4. **grub 用 `root=/dev/sda2` 非 UUID** → 补装 initramfs-tools 后重跑 `update-grub` 自动改成 UUID
5. **新系统极简缺 Qt 运行库**（libGL/libEGL/libxcb/libxkbcommon/libfontconfig）→ apt 补全套 + fonts-noto-cjk
6. **重装后 SSH host key 变** → mac 端 `ssh-keygen -R 192.168.0.53`
7. **fresh 系统 sudo 要密码** → 写 `/etc/sudoers.d/ubuntu-nopass` 设 NOPASS

---

## 下一步（可选）
- 装 VNC/X（如 `tigervnc` 或 xrdp）让能实连设备看实时扫频界面
- DNB1101 阻抗固件调试见 `BUILD_RECORD_2026-05-29_DNB_SW_DEBUG.md`（J-Link V9.5 + CP2102 /dev/ttyUSB0 + Saleae 都在这台）
