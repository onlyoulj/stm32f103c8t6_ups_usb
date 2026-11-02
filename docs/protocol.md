# 通信协议

Type-C 口枚举为 USB CDC ACM（虚拟串口），Linux 下通常是 `/dev/ttyACM0`，Windows 下是 `COMx`。串口参数填 115200 8N1 即可；CDC 实际不依赖波特率。

设备不会主动推送数据。主机发一行命令，设备回一行（或对 Megatec 动作命令不回包）。所有数值都是整数，单位写在字段名里。

- Megatec 命令：以 `\r` 结尾，回包以 `\r` 结尾；
- 扩展命令：以 `\r\n` 或 `\n` 结尾，回包以 `\r\n` 结尾；
- 单行最长 127 字符；超长行会被丢弃；
- 可同时接 NUT 或上位机之一，**不要两个进程同时打开同一个串口**。

---

## 1. Megatec / Q1（NUT 兼容）

### 1.1 `Q1` 状态查询

请求：

```text
Q1\r
```

回包：

```text
(MMM.M NNN.N PPP.P QQQ RR.R SS.SS TT.T b7b6b5b4b3b2b1b0\r
```

示例（默认 2S 配置）：

```text
(012.0 012.0 011.9 038 50.0 07.81 25.3 00001001\r
```

| 字段 | 含义 | 本机说明 |
|---|---|---|
| `MMM.M` | 输入电压 | 单位 V；电池供电时为实际输入（通常 0） |
| `NNN.N` | 输入故障电压 | 掉电瞬间的输入电压，保持到下一次查询/恢复 |
| `PPP.P` | 输出电压 | 单位 V |
| `QQQ` | 输出电流百分比 | 0–100；有输出 INA219 时为实测，否则为估算 |
| `RR.R` | 输入频率 | DC 输入，固定回报 `nominal_freq_x10`（默认 50.0） |
| `SS.SS` | 电池电压 | 单位 V，两位小数 |
| `TT.T` | 温度 | 0.1 °C；NTC 无效时回 `0.0` |
| `b7…b0` | 状态位 | 见下表 |

状态位（**b7 在最左边**）：

| 位 | 名称 | 1 的含义 |
|---|---|---|
| b7 | Utility Fail | 市电失败/在电池上 |
| b6 | Battery Low | 电池低；**只在电池供电时置 1**，避免主机在充电时误关机 |
| b5 | Bypass/Boost | 升压/旁路工作（本机在电池供电时置 1） |
| b4 | UPS Failed | 存在硬故障（过温、过流、输出异常等） |
| b3 | Standby | 本机固定为 1（待机式/离线式 UPS） |
| b2 | Test in Progress | 电池切换自检中 |
| b1 | Shutdown Active | 已计划/正在执行关机 |
| b0 | Beeper On | 蜂鸣器使能 |

NUT 的 `blazer`/`nutdrv_qx` 解析要求：

- 回包长度至少 46 字节；本机 48 字节；
- 7 个数值域用空格分隔，最后 8 位状态；
- 以 `(` 开头、`\r` 结尾。

### 1.2 `F` 额定值

```text
F\r
#012.0 004 007.4 50.0\r
```

字段：输入标称电压、输入标称电流（A）、电池标称电压、输入标称频率。NUT 用电池标称电压来推算“几节电池”，所以这里填的是 **整组标称电压**（2S=7.4，3S=11.1，4S LiFePO4=12.8）。

### 1.3 `I` 厂商信息

固定 39 字节（含 CR）：

```text
#XiaoBuUPS      DC-UPS-12V 1.0.0     \r
```

字段长度：1 字节 `#` + 15 字节厂商 + 空格 + 10 字节型号 + 空格 + 10 字节固件版本 + CR。NUT 的 `blazer_vendor` 按固定宽度解析。

### 1.4 动作命令

| 命令 | 作用 | 回包 |
|---|---|---|
| `Q` | 切换蜂鸣器使能 | 无 |
| `C` | 取消关机计划；若输出已切断，则至少保持 10 s 后恢复 | 无 |
| `CT` | 取消电池自检 | 无 |
| `T` | 快速自检（10 s） | 无；不支持时回显命令 |
| `TL` | 自检直到低电量 | 无；不支持时回显 |
| `T<n>` | 自检 n 分钟（01–99） | 无；不支持时回显 |
| `S<n>` | n 分钟后切断输出 | 无；解析失败时回显 |
| `S<n>R<m>` | n 分钟后切断，m 分钟后恢复 | 无；解析失败时回显 |
| 未知命令 | 原样回显 + CR | 用于 NUT 判断失败 |

`S` 的时间格式：

- `.2`–`.9`：0.2–0.9 分钟（12–54 s）；
- `00`–`10`：0–10 分钟（两位数字；`S00` 立即切断）；
- `R` 后 `0000`–`9999` 分钟；`R0000` 表示不自动恢复；
- NUT 的 `load.off` 使用 `S00R0000`，`load.on` 使用 `C`；
- 本机 `S<n>` **不带 `R`** 时，输出保持关闭，直到 `C`/按键；不会仅因市电存在就自动开启。需要自动恢复请用 `S<n>R<m>`；
- 若 `R<m>` 到期时市电未恢复，输出会等到市电恢复并再保持 10 s 后才开启；
- 若电池先到低电量，按低电量保护逻辑提前切断。

> 真实电池自检需要可选的 AC_CUT 硬件。没有该硬件时，`T/TL/T<n>` 会回显命令，NUT 会把它标记为失败；这是有意的，不会“假装测试成功”。

### 1.5 NUT 配置

```ini
[stm32ups]
    driver = nutdrv_qx
    port = /dev/stm32-ups
    protocol = megatec
    pollinterval = 5
```

udev 规则、`upsmon` 示例和排错见 `host/nut/README.md`。

---

## 2. 扩展 JSON 命令

命令第一个字符是 `X`，不区分大小写；参数用空格分隔。

成功回包：

```json
{"ok":true,...}
```

失败回包：

```json
{"ok":false,"error":"..."}
```

### 2.1 `XID`

```text
XID
{"ok":true,"device":"STM32-DC-UPS","mfr":"XiaoBuUPS","fw":"1.0.0",
 "proto":1,"board":"LCKFB-DKX-STM32F103C8T6","transport":"USB CDC"}
```

### 2.2 `XSTATUS`

```text
XSTATUS
```

示例：

```json
{
  "ok": true,
  "device": "STM32-DC-UPS",
  "fw": "1.0.0",
  "proto": 1,
  "uptime_s": 1234,
  "state": "battery",
  "state_code": 2,
  "ac": false,
  "output": true,
  "charging": false,
  "charge_done": false,
  "battery_present": true,
  "test": false,
  "shutdown_pending": false,
  "beeper": true,
  "vin_mv": 0,
  "vin_fault_mv": 11980,
  "vout_mv": 11960,
  "vbat_mv": 7810,
  "ibat_ma": -420,
  "iout_ma": 760,
  "temp_c_x10": 253,
  "soc_x10": 873,
  "runtime_s": 6100,
  "load_pct": 38,
  "faults": 0,
  "warnings": 0,
  "low": false,
  "critical": false,
  "ac_fail_count": 2,
  "shutdown_remaining_s": 0,
  "restore_remaining_s": 0,
  "ina_ok": true,
  "ntc_ok": true,
  "output_sensor": true,
  "config_dirty": false
}
```

字段说明：

| 字段 | 单位/取值 |
|---|---|
| `state` | `boot` / `online` / `battery` / `low_battery` / `shutdown` / `fault` |
| `state_code` | 0–5，对应上表顺序 |
| `ac/output/charging/charge_done/...` | 布尔 |
| `vin_mv/vout_mv/vbat_mv/vin_fault_mv` | mV |
| `ibat_ma` | mA，**正=充电，负=放电** |
| `iout_ma` | mA，输出侧；没有输出 INA219 时是估算值 |
| `temp_c_x10` | 0.1 °C；`9999` 表示温度不可用 |
| `soc_x10` | 0–1000，即 0.0–100.0 % |
| `runtime_s` | 预计剩余秒数；0 表示未知/输出关闭 |
| `load_pct` | 0–100；没有输出 INA219 时由假设负载估算 |
| `faults` / `warnings` | 位掩码，见下 |
| `low/critical` | 电池模型的原始低电量/临界标志 |
| `ac_fail_count` | 累计市电掉电次数（NVM 持久化） |
| `shutdown_remaining_s` | 距离切断输出还有多少秒；0 表示没有计划 |
| `restore_remaining_s` | 距离恢复输出还有多少秒；0 表示没有计划 |
| `ina_ok/ntc_ok/output_sensor` | 对应传感器是否可用 |
| `config_dirty` | 配置有未保存修改 |

故障位（`faults`）：

| 位 | 含义 |
|---|---|
| 0 | `FAULT_BATTERY_UV`（硬欠压） |
| 1 | `FAULT_BATTERY_OV` |
| 2 | `FAULT_BATTERY_MISSING` |
| 3 | `FAULT_OVER_CURRENT` |
| 4 | `FAULT_OVER_TEMP` |
| 5 | `FAULT_OUTPUT_UV` |
| 6 | `FAULT_OUTPUT_OV` |
| 7 | `FAULT_INPUT_OV` |
| 8 | `FAULT_SENSOR` |
| 9 | `FAULT_CHARGER` |
| 10 | `FAULT_INTERNAL` |

> 说明：当前固件把电池硬欠压、过压、充电器问题主要放在 `warnings`，`faults` 表示会切断输出的硬故障；具体以 `host/ups_monitor.py` 的映射为准。

告警位（`warnings`）：

| 位 | 含义 |
|---|---|
| 0 | 电池过压 |
| 1 | 温度禁止充电 |
| 2 | 温度降额/放电温度越界 |
| 3 | 充电器长时间无状态 |
| 4 | 电流传感器不可用 |
| 5 | 温度传感器不可用 |
| 6 | 输出电流传感器不可用 |
| 7 | 配置未保存/无效 |
| 8 | 电池缺失 |
| 9 | 输入低 |
| 10 | 充电过流 |
| 11 | 电池电压采样异常 |
| 12 | NVM 写入失败 |

### 2.3 `XCONFIG`

返回当前配置和 `dirty` 标志。示例（节选）：

```json
{"ok":true,"fw":"1.0.0","dirty":false,
 "cells":2,"capacity_mah":5000,"low_pct":20,"critical_pct":5,
 "cutoff_grace_s":180,"ac_restore_s":3,
 "cell_nominal_mv":3700,"cell_full_mv":4200,"cell_empty_mv":3000,
 "cell_low_mv":3450,"cell_critical_mv":3250,"cell_ov_mv":4300,
 "vbat_scale":1000,"vbat_offset":0,"vin_scale":1000,"vin_offset":0,
 "vout_scale":1000,"vout_offset":0,"shunt_mohm":20,
 "max_charge_ma":2000,"max_discharge_ma":6000,
 "max_load_ma":2000,"assumed_load_ma":800,
 "vin_present_mv":8000,"vin_nominal_mv":12000,"vin_max_mv":16000,
 "vout_nominal_mv":12000,"vout_uv_mv":10500,"vout_ov_mv":13500,
 "charge_max_temp":450,"discharge_min_temp":-100,
 "discharge_max_temp":600,"temp_fault":700,"temp_offset":0,
 "nominal_freq_x10":500,"beeper_enabled":1,"ntc_enabled":1,
 "ina_invert":0,"auto_recover":1}
```

### 2.4 `XSET key=value`

只修改 RAM，返回 `saved:false`。修改后：

- **60 秒**内无操作自动保存；或
- 立即发送 `XSAVE`。

```text
XSET low_pct=25
{"ok":true,"key":"low_pct","saved":false}
```

非法键/值：

```json
{"ok":false,"error":"unknown_key"}
```

### 2.5 `XSAVE` / `XRESET`

```text
XSAVE
{"ok":true}

XRESET
{"ok":true,"reset":true}
```

- `XSAVE` 立即写入双页 NVM；
- `XRESET` 恢复默认值并立即保存；计数器、SoC 状态也重置；建议确认后再执行。

### 2.6 `XSHUTDOWN` / `XCANCEL`

```text
XSHUTDOWN <seconds> [restore_seconds]
XCANCEL
```

- 关机延迟 0–86400 s；恢复延迟 0–604800 s（7 天，覆盖 Megatec 的 R0001–R9999 分钟）；
- `restore_seconds` 可选，0 表示不自动恢复；
- 切断后如果市电不在，恢复会等到市电恢复并再保持 10 s；
- `XCANCEL` 取消计划；如果输出已经切断，至少保持 10 s 再恢复。

示例：

```text
XSHUTDOWN 60 300
{"ok":true,"shutdown_in_s":60,"restore_in_s":300}
```

> 如果主机本身由 UPS 供电，`XSHUTDOWN 0` 会让主机立刻掉电；先让主机完成关机，再切断。JSON 回包会先写入 USB，但主机可能来不及处理。

### 2.7 `XTEST` / `XBEEP` / `XHELP`

```text
XTEST <seconds|low>
```

- 需要 `UPS_HAS_AC_CUT=1` 的硬件；否则返回 `test_not_supported`；
- `XTEST low` 运行到低电量；
- 单次测试最长 1 小时；
- 开始前要求：市电存在、电池存在、SoC ≥ 30%、无故障、输出未关闭。

```text
XBEEP
{"ok":true,"beeper":false}
```

切换蜂鸣器使能。

```text
XHELP
{"ok":true,"cmds":[...]}
```

---

## 3. 配置键

| 键 | 默认 | 说明/范围 |
|---|---|---|
| `cells` | 2 | 串数 1–6 |
| `capacity_mah` | 5000 | 500–60000 |
| `low_pct` | 20 | 低电量告警 %；必须大于 `critical_pct` |
| `critical_pct` | 5 | 临界电量 % |
| `cutoff_grace_s` | 180 | 临界后切断延时 0–3600 s |
| `ac_restore_s` | 3 | 市电稳定多久才转回 1–60 s |
| `cell_nominal_mv` | 3700 | 单体标称 |
| `cell_full_mv` | 4200 | 单体充满 |
| `cell_empty_mv` | 3000 | 单体放空 |
| `cell_low_mv` | 3450 | 放电低电压 |
| `cell_critical_mv` | 3250 | 放电临界电压 |
| `cell_ov_mv` | 4300 | 单体过压，禁止充电 |
| `vbat_scale` | 1000 | 电池电压校准 500–2000 |
| `vbat_offset` | 0 | mV，±5000 |
| `vin_scale` | 1000 | 输入校准 |
| `vin_offset` | 0 | mV |
| `vout_scale` | 1000 | 输出校准 |
| `vout_offset` | 0 | mV |
| `shunt_mohm` | 20 | INA219 分流电阻 mΩ；0 关闭电流检测 |
| `max_charge_ma` | 2000 | 充电过流阈值 |
| `max_discharge_ma` | 6000 | 放电过流阈值 |
| `max_load_ma` | 2000 | 输出额定电流/负载百分比基准 |
| `assumed_load_ma` | 800 | 无输出传感器时的假设负载 |
| `vin_present_mv` | 8000 | 市电存在阈值 |
| `vin_nominal_mv` | 12000 | 输入标称（F 命令） |
| `vin_max_mv` | 16000 | 输入过压 |
| `vout_nominal_mv` | 12000 | 输出标称（信息） |
| `vout_uv_mv` | 10500 | 输出欠压故障 |
| `vout_ov_mv` | 13500 | 输出过压故障 |
| `charge_max_temp` | 450 | 充电最高温度 0.1 °C；低于 0 °C 也禁充 |
| `discharge_min_temp` | -100 | 放电最低温度 0.1 °C |
| `discharge_max_temp` | 600 | 放电最高温度 |
| `temp_fault` | 700 | 过温切断 |
| `temp_offset` | 0 | NTC 校准 ±20 °C |
| `nominal_freq_x10` | 500 | Q1 回报频率，45.0–65.0 Hz |
| `beeper_enabled` | 1 | 0/1 |
| `ntc_enabled` | 1 | 0/1；不接 NTC 时必须设 0 |
| `ina_invert` | 0 | 1 = 正电流方向反过来 |
| `auto_recover` | 1 | 过流/输出故障是否自动重试；0 = 只长按复位 |

校验规则：单体阈值必须满足 `empty < critical < low < full < ov`；`vin_present < vin_nominal < vin_max`；`vout_uv < vout_nominal < vout_ov`；`discharge_min < discharge_max < temp_fault`。不满足的值会被拒绝，原配置不变。

---

## 4. 错误码

| error | 含义 |
|---|---|
| `missing_equals` | `XSET` 缺少 `=` |
| `missing_key` | 缺少键 |
| `key_too_long` | 键超过 31 字符 |
| `not_an_integer` | 值不是十进制整数 |
| `unknown_key` | 未知配置键 |
| `invalid_value` / 具体校验文本 | 越界或交叉约束失败 |
| `time_out_of_range` | 关机延迟超过 86400 s 或恢复延迟超过 604800 s |
| `bad_seconds` / `bad_restore` / `trailing_args` | `XSHUTDOWN` 参数错误 |
| `bad_test_duration` | `XTEST` 参数错误 |
| `test_not_supported` | 没装 AC_CUT 硬件 |
| `test_busy` | 已有测试 |
| `no_ac` / `no_battery` / `battery_low` / `not_ready` | 自检条件不满足 |
| `unknown_command` | 未知 `X...` 命令 |
| `response_too_large` | JSON 超出内部缓冲（正常不会发生） |
| `nvm_write_failed` | Flash 写入失败 |

Megatec 动作命令解析失败时不是 JSON，而是原样回显命令行，方便 NUT 判失败。

---

## 5. 示例脚本

```sh
# 查看
python host/ups_monitor.py --port COM7

# 改配置并保存
python host/ups_monitor.py --set cells=3 --set capacity_mah=4000 --save

# 查看 JSON
python host/ups_monitor.py --port COM7 --json

# 60 秒后关机，5 分钟后恢复
python host/ups_monitor.py --shutdown-after 60 --restore-after 300 --yes

# 取消
python host/ups_monitor.py --cancel

# NUT
upsc stm32ups
```

协议版本：`UPS_PROTO_VERSION = 1`；固件版本：`UPS_FW_VERSION = "1.0.0"`。
