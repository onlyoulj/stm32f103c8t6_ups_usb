# 主机侧测试

这些测试不需要 STM32 硬件，用 g++ 编译真实的纯逻辑/协议代码：

```sh
# Windows
run_tests.cmd

# Linux/macOS
./run_tests.sh
```

覆盖：

- `settings.cpp`：默认值、校验、键值应用、CRC；
- `battery.cpp`：库仑计、OCV、低电量/临界、运行时；
- `protocol_format.cpp`：Q1、F、I 的字段顺序、宽度、状态位和 NUT 最小长度；
- `comm.cpp`：用假 `Stream` 驱动真实协议解析器，覆盖 Q1/F/I、Q/T/C/CT、S 命令、T 命令、XSTATUS/XCONFIG/XSET/XSAVE/XRESET/XSHUTDOWN/XTEST/XBEEP、错误处理。

注意：`test/stubs/Arduino.h` 只用于主机测试，不会被固件编译；固件使用真正的 STM32duino 头文件。
