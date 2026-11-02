# 主机侧工具

| 文件 | 用途 |
|---|---|
| `ups_gui.py` | Windows 图形上位机（Python + tkinter） |
| `dist/STM32-UPS-Monitor.exe` | 打包好的 Windows 上位机（构建产物） |
| `ups_monitor.py` | 命令行监控/配置 |
| `ups_guard.py` | 低电量时执行主机关机的守护进程 |
| `nut/` | NUT `nutdrv_qx` + udev 配置 |
| `systemd/` | `ups_guard.py` 的 systemd 示例 |

依赖只有 `pyserial`（`pip install -r host/requirements.txt`）。GUI 使用的 `tkinter` 随 python.org 的 Python 一起安装，不需要额外包。

## Windows 图形上位机

### 运行

```sh
# 打包版（无需 Python）
host\dist\STM32-UPS-Monitor.exe

# 源码运行
python host/ups_gui.py
python host/ups_gui.py --port COM23
```

不指定端口时会优先选择 `0483:5740` 的 STM32 CDC 口。插拔设备后点“刷新”即可重新枚举；连接中断且勾选“自动重连”时，程序每 2 秒重试一次。

### 四个页面

1. **实时监控**：状态横幅（市电/电池/低电量/故障）、电量与续航、8 个数据卡片（输入/输出/电池电压、电流、负载、温度、续航、掉电次数）、故障与告警位、输入/输出/电池电压曲线、电量/负载曲线。
2. **配置**：39 个参数按 8 组展示，含单位与范围校验；支持“从设备读取”“应用修改（XSET）”“写入 NVM（XSAVE）”“恢复出厂（XRESET）”。
3. **操作**：蜂鸣器切换、电池自检（10 秒 / 60 秒 / 到低电量）、计划切断与取消，并显示设备身份信息。
4. **日志**：连接、状态变化和命令记录；可显示协议报文（`→ XSTATUS` / `← {...}`），可导出日志，可按 1 Hz 把状态写入 CSV。

### 行为说明

- 配置修改先写入设备 RAM。60 秒内无操作设备会自动保存；也可以点“写入 NVM”立即保存。
- 计划切断默认延迟 60 秒、300 秒后恢复。**如果本机由 UPS 供电，`XSHUTDOWN 0` 会立即让本机掉电**，请先完成本机关机。
- 自检需要可选的 `AC_CUT` 硬件；没有时设备返回 `test_not_supported`，界面会弹窗说明，不会假装成功。
- 同一个串口不要同时被 NUT、`ups_monitor.py` 和本程序占用。
- 退出时会把端口、刷新间隔、窗口位置等保存到 `%APPDATA%\STM32-UPS-Monitor\settings.json`。
- 程序声明了 DPI 感知，在 125%/150% 缩放下文字不会发虚。

### 打包 exe

```sh
python tools/make_icon.py
python -m PyInstaller --noconfirm --clean --onefile --windowed ^
  --name STM32-UPS-Monitor --icon host/ups.ico ^
  --add-data "host/ups.ico;." --paths host ^
  --distpath host/dist --workpath host/build --specpath host host/ups_gui.py
```

产物在 `host/dist/STM32-UPS-Monitor.exe`，约 12 MB，自带 Python 运行时。exe 未做代码签名，首次运行时 Windows SmartScreen 可能提示，选择“更多信息 → 仍要运行”即可。

### 冒烟测试

```sh
python host/ups_gui.py --port COM23 --smoke-test --smoke-seconds 8
```

程序会自动连接、采集若干秒后退出：成功时 stdout 打印一行 `SMOKE {"ok": true, ...}`，退出码为 0；失败为 1。打包后的 exe 同样支持这三个参数，但 `--windowed` 没有控制台，只能通过退出码判断。

### 常见问题

| 现象 | 排查 |
|---|---|
| 端口列表里没有设备 | Type-C 必须是数据线；确认设备管理器里出现“USB 串行设备”；点“刷新” |
| 打开串口失败 / 被占用 | 关闭 NUT、`ups_monitor.py`、串口助手，再重试 |
| 界面显示“故障 / 电池未连接” | 裸板未接电池、INA219、NTC 时会这样，属保护逻辑正常动作 |
| 曲线没有变化 | 确认已连接且“最后更新”时间在走；设备 1 秒应答一次 |
| exe 被杀毒软件拦截 | PyInstaller onefile 常见，加白名单或改用源码运行 |

## 命令行工具

```sh
python host/ups_monitor.py --port COM7
python host/ups_monitor.py --port COM7 --watch
python host/ups_monitor.py --port COM7 --json
python host/ups_monitor.py --set low_pct=25 --save
python host/ups_monitor.py --shutdown-after 60 --restore-after 300 --yes
```

NUT 配置和 udev 规则见 `host/nut/README.md`；低电量关机守护见 `host/ups_guard.py`（默认 dry-run，加 `--execute` 才执行关机命令）。
