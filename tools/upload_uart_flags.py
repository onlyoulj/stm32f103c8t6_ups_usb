# PlatformIO extra script for the STM32F103 factory USART1 bootloader.
#
# stm32flash's default `-w` mass-erases the whole flash and does not verify.
# This firmware keeps its two power-fail-safe settings pages at 0x0800F800 and
# 0x0800FC00 (see ldscript/stm32f103c8_nvm.ld), so limit the erase to the
# 62 KiB application area and verify the image after writing.
#
# The 62-page limit assumes the 64 KiB / 1 KiB-page STM32F103C8T6 with the two
# reserved NVM pages.  Update it if the linker script layout changes.
Import("env")

if env.GetProjectOption("upload_protocol", "") == "serial":
    env.Prepend(UPLOADERFLAGS=["-e", "62", "-v"])
