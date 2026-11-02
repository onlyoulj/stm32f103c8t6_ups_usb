# 预编译固件

- 版本：`1.0.0`
- 构建日期：2026-11-02
- 目标：STM32F103C8T6 / 立创·地阔星，Arduino Core STM32 + LTO
- 默认配置：2S Li-ion、5000 mAh、12 V 输出、UART ISP 上传
- Flash 占用：54,948 字节（应用区上限 62 KiB，最后 2 KiB 为 NVM）

| 文件 | 用途 |
|---|---|
| `stm32f103c8_ups_v1.0.0.bin` | 写入 `0x08000000`，UART ISP / ST-Link / CubeProgrammer 均可 |
| `stm32f103c8_ups_v1.0.0.hex` | 带地址，适合 STM32CubeProgrammer / 通用烧录器 |

校验：

```text
SHA256 (bin) 14ecaad39204a4d727e8067cb81e838873d5364a3cd1ba7f7ddbadb449c04397
SHA256 (hex) 43a5868a76254ed88064a752ad9d65a2a818aff70d7b456069905932b42e9f4d
```

## 烧录注意

- UART ISP：BOOT0=1、BOOT1=0，RESET，然后 `pio run -t upload --upload-port COMx`；烧完 BOOT0=0，再 RESET。详见根目录 `FLASH_UART.md`。
- 使用 `stm32flash` 手动烧录时保留 NVM 页：
  ```sh
  stm32flash -e 62 -v -g 0x08000000 -b 115200 -w firmware/stm32f103c8_ups_v1.0.0.bin COMx
  ```
- 用 STM32CubeProgrammer 烧 `.hex` 时，注意其“全片擦除”选项会清掉 NVM 配置。
- 这是默认 **2S** 配置的固件。接电池前必须根据实际电芯/串数/充电器修改阈值并 `XSAVE`，否则可能误报过压或错误切断。
- 预编译固件不替代 `docs/hardware.md` 的 bring-up 和安全检查；未在真实功率级上验证。

## 重新构建

```sh
pio run
```

然后用 `arm-none-eabi-objcopy` 从 `.elf` 生成 `.hex`，或直接使用 PlatformIO 的 `.pio/build/bluepill_f103c8/firmware.bin`。
