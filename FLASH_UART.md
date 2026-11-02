# 用 UART 烧录（不需要 ST-Link）

`platformio.ini` 当前配置为：

```ini
upload_protocol = serial
upload_speed = 115200
```

STM32F103C8T6 出厂系统存储器里有一个 **USART1 引导程序**，可以通过 PA9/PA10 烧录。烧录时用的是这个引导程序，不是 UPS 固件里的调试串口；烧完运行用户程序后，PA9/PA10 才是可选的调试串口（默认编译关闭）。

## 接线

需要一个 **3.3V 电平** 的 USB-TTL 适配器（CH340、CP2102、FT232 等）：

| USB-TTL | 开发板 |
|---|---|
| TXD | PA10（USART1_RX） |
| RXD | PA9（USART1_TX） |
| GND | GND |
| 3.3V | 仅台架测试时给板子供电；不要和 UPS 的 5V 同时供电 |

注意：

- **适配器必须是 3.3V 电平**，不要用 5V TTL 直连 PA9/PA10。
- TX/RX 是交叉的：适配器 TX 接板子 RX（PA10）。
- 如果板子已经由 UPS 的 5V/3.3V 供电，只接 TX/RX/GND 三根线。
- Type-C 口是 USB FS 数据口，**不能**用于这个出厂 UART 引导程序；必须用 PA9/PA10 排针。

## 进入 ISP 模式

1. 把 BOOT0 跳线设为 **1**，BOOT1 设为 **0**；
2. 按一次 RESET（或断电再上电）；
3. 这时 MCU 运行系统存储器里的引导程序，等待 UART 烧录；
4. 烧录完成后，BOOT0 改回 **0**，再按 RESET，运行用户固件。

地阔星板子上的 BOOT0/BOOT1 跳线位置以你手上的板子丝印为准；BOOT0=0 时 BOOT1 不影响从用户 Flash 启动。

## 用 PlatformIO 烧录

先确认串口：

```sh
pio device list
```

然后：

```sh
pio run -t upload --upload-port COM7
```

PlatformIO 会使用 `stm32flash`（首次会自动下载 `tool-stm32flash` 包），实际命令是：

```sh
stm32flash -e 62 -v -g 0x08000000 -b 115200 -w .pio/build/bluepill_f103c8/firmware.bin COM7
```

- `-e 62`：只擦前 62 个 1 KiB 页，保留最后 2 KB 的 NVM 配置页（0x0800F800/0x0800FC00）。`stm32flash -w` 默认是整片擦除，手动执行时不要漏掉这个参数。
- `-v`：写入后回读校验；PlatformIO 的串口上传默认不带校验，本工程通过 `tools/upload_uart_flags.py` 加上了。
- 如果以后改了链接脚本或 NVM 页布局，同步修改 `tools/upload_uart_flags.py` 里的页数 62。
- 把 `COM7` 换成实际端口（Linux 下是 `/dev/ttyUSB0`）。
- 如果 115200 不稳定，把 `platformio.ini` 的 `upload_speed` 改成 `57600` 再试。
- Linux 下当前用户要在 `dialout` 组，或用 `sudo`。
- 如果 `tool-stm32flash` 没有自动安装：`pio pkg install -g -t "platformio/tool-stm32flash@~0.7.0"`。

## 用 STM32CubeProgrammer 烧录

如果不使用 PlatformIO 的 upload：

1. BOOT0=1、BOOT1=0，RESET；
2. STM32CubeProgrammer 选择 **UART**，端口选 USB-TTL 的 COM 口；
3. 参数：**115200、8 data bits、Even parity、1 stop bit**；
4. 选择 `.pio/build/bluepill_f103c8/firmware.bin`，地址 `0x08000000`；
5. Download；
6. BOOT0=0，RESET。

> STM32 的 UART 引导程序使用 **偶校验**；CubeProgrammer 会自动配置。手动用串口工具时不要用 8N1。
>
> CubeProgrammer 默认可能整片擦除，会清掉最后 2 KB 的 NVM 设置；需要保留设置时优先用上面的 PlatformIO 命令。

## 常见问题

| 现象 | 排查 |
|---|---|
| `Failed to init device` / 无响应 | BOOT0 是否为 1；是否按过 RESET；TX/RX 是否交叉；适配器是否 3.3V |
| 烧录后程序不运行 | BOOT0 是否恢复 0；是否 RESET；Flash 是否从 0x08000000 写入 |
| 端口被占用 | 关闭串口助手/NUT/`ups_monitor.py` 后重试 |
| 115200 失败 | 改 57600；缩短杜邦线；确保共地 |
| 找不到 stm32flash | 重新执行 `pio run -t upload` 让 PlatformIO 安装；或手动安装 `stm32flash` |
| 上传后配置丢失 | `stm32flash -w` 默认整片擦除；本工程用 `-e 62` 保留最后 2 KB NVM 页。手动执行时漏掉 `-e 62`，或用 CubeProgrammer 整片擦除，都会清掉设置 |

## 用 Type-C 调试

烧录完成后，Type-C 是 UPS 的 USB CDC 通信口：

```sh
pio device list          # 应看到一个 STM32 的 CDC 串口
python host/ups_monitor.py --port COMx
```

它不是 ST-Link，也不是 DFU；不能用来烧录。通信协议见 `docs/protocol.md`。
