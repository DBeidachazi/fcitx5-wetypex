# 故障排查

## 输入法没有出现在列表中

确认以下文件来自同一个系统级安装：

```text
/usr/lib/fcitx5/wetypex.so
/usr/share/fcitx5/addon/wetypex.conf
/usr/share/fcitx5/inputmethod/wetypex.conf
```

运行 `fcitx5-remote -r`。仍然不可见时检查：

```bash
fcitx5-diagnose | less
```

## 提示核心不可用

```bash
fcitx5-wetypex-setup --check
```

输出中的 `core_ready` 应为 `true`。若报告文件缺失或核心验证失败，重新准备固定版本：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip
```

散列不匹配时不要绕过校验。下载或版本变化可能意味着 ABI 不兼容。

## 本地候选正常，但没有云候选

1. 在设置中确认“单机模式”关闭。
2. 运行 `fcitx5-wetypex-account group-info`。
3. 检查 `~/.local/share/fcitx5-wetypex/account/identity.json` 是否存在。
4. 查看 `account/last-network.log` 中最后一次业务初始化结果，分享日志前删除账户标识。

## 跨设备剪贴板没有同步

确认设备组的 `func_switch` 包含剪贴板位 `1`。Wayland 需要 `wl-copy` 与 `wl-paste`；X11 需要可用的 Qt 剪贴板。同步守护进程使用文件锁，避免手工同时运行多个实例。

## 语音没有识别结果

先验证默认输入设备：

```bash
pw-record --rate 16000 --channels 1 --format s16 /tmp/wetypex-test.wav
```

录制几秒后按 `Ctrl+C`，确认 WAV 非空。随后检查 `state/voice/last-record.log`。识别需要联网、有效账户身份和 FFmpeg 的 Opus 编码器。

## AI 窗口空白或缺图标

重新提取界面资源：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

Qt WebEngine 无法启动时，检查 `qt6-webengine`、图形驱动和桌面沙箱配置。AI 结果保存在 `~/.local/share/fcitx5-wetypex/ai/`，其中可能含有用户问题，不应直接公开。

## 隔空传送无法连接

先运行：

```bash
fcitx5-wetypex-setup --check
```

检查结果应同时包含 `flurry` 与 `wxp2p`，并且 `core_ready` 为 `true`。原版 WXP2P 会依次尝试局域网直连、公网打洞和腾讯中继，因此两台设备不必位于同一局域网。受限网络需要允许出站 UDP，以及到上游中继的 TCP 80、8080 和 16285 端口。

传输日志位于 `~/.local/share/fcitx5-wetypex/state/transfer/transport.log`。手机点击重试时桌面窗口无需关闭；WeTypeX 会继续轮询同一传输码，并在官方服务返回新调度数据后重建会话。若只缺少传输图标或连接示意图，可从同一官方安装包重新提取：

```bash
fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip --icons-only
```

## 重新配对

普通故障不要删除身份文件。确需重置时先备份：

```bash
cp -a ~/.local/share/fcitx5-wetypex/account ~/wetypex-account-backup
```

再关闭 Fcitx5 与 WeTypeX 相关进程，移走 `account` 和 `state/sync-state.json`，重新打开设置窗口获取匹配码。
