# AGENTS.md — 小智 AI 聊天机器人 (xiaozhi-esp32)

> ESP-IDF v2 项目。基于 MCP 协议的多模态 AI 聊天机器人，支持 70+ 硬件板型。

## 构建命令

```bash
# 设定目标芯片 (esp32, esp32s3, esp32c3, esp32c5, esp32c6, esp32p4)
idf.py set-target esp32s3

# 菜单配置（选择板型 BOARD_TYPE、语言等）
idf.py menuconfig

# 构建
idf.py build

# 烧录 + 串口监控
idf.py flash monitor
```

**注意**: 这是 ESP-IDF v5.x 项目，使用 `idf.py` 而非 `make`。需要先 `export.sh` / `export.ps1` 设置环境。

## 项目架构

```
main/
├── CMakeLists.txt          # 源文件注册 + BOARD_TYPE → 板级目录映射
├── Kconfig.projbuild       # Kconfig 菜单（板型选择、语言、OTA等）
├── main.cc                 # 入口
├── application.{h,cc}      # Application 单例，设备状态机
├── device_state_machine.{h,cc}  # 设备状态定义与回调
├── mcp_server.{h,cc}       # 本地 MCP 工具注册
├── settings.{h,cc}         # NVS 设置读写
├── boards/
│   ├── common/             # 基类 (board, wifi_board, dual_network_board, ml307_board...)
│   └── <board-type>/       # 各板型实现（config.h + *.cc）
├── audio/
│   ├── audio_codec.h       # AudioCodec 抽象接口
│   └── codecs/             # 编解码器实现 (box_audio_codec, es8311, es8388, no_audio_codec...)
├── display/                # 显示系统 (LcdDisplay, OledDisplay, EmoteDisplay)
├── led/                    # LED 控制 (SingleLed, CircularStrip, GpioLed)
└── protocols/              # 通信协议 (MQTT, WebSocket, UDP)
```

### 关键基类层次

```
Board  (boards/common/board.h)
├── WifiBoard        (wifi_board.h)    — 纯 WiFi 板
├── Ml307Board       (ml307_board.h)   — 纯 4G (ML307) 板
├── Nt26Board        (nt26_board.h)    — 纯 4G (NT26) 板
└── DualNetworkBoard (dual_network_board.h) — WiFi/4G 双模切换板
```

### 板级注册机制

1. **Kconfig** (`main/Kconfig.projbuild`) — 在 `choice BOARD_TYPE` 中添加 `config BOARD_TYPE_xxx`
2. **CMake** (`main/CMakeLists.txt`) — 在 `if(CONFIG_BOARD_TYPE_xxx)` 分支设置 `BOARD_TYPE` 字符串
3. **源文件自动发现** — CMake 使用 `file(GLOB ... boards/${BOARD_TYPE}/*.cc)` 自动收集板级源文件，无需手动列出

## 添加新板型的步骤

参见 [docs/custom-board_zh.md](docs/custom-board_zh.md) 了解完整流程。简要步骤：

1. 创建 `main/boards/<board-name>/config.h` — 引脚定义
2. 创建 `main/boards/<board-name>/xxx_board.cc` — 继承合适的 Board 基类，实现：
   - `GetAudioCodec()` — 返回 AudioCodec 实例
   - `GetDisplay()` — 返回 Display 实例
   - `GetBacklight()` — （可选）返回 Backlight 实例
   - `GetBatteryLevel()` — （可选）电量检测
   - 构造函数中初始化 I2C、SPI、按键等外设
   - 文件末尾添加 `DECLARE_BOARD(ClassName)` 宏
3. 在 `main/Kconfig.projbuild` 的 `choice BOARD_TYPE` 中添加选项
4. 在 `main/CMakeLists.txt` 中添加 `elseif(CONFIG_BOARD_TYPE_xxx)` 映射

## 关键约定

- **采样率**: 通常 24000Hz（双工 I2S），部分 C3 板用 16000Hz
- **音频编解码**: 使用 ES8311+ES7210 的板继承 `BoxAudioCodec`，简单 DAC+MIC 用 `NoAudioCodec`
- **I2C 总线**: 新版 ESP-IDF 使用 `i2c_new_master_bus()` (i2c_master_bus_handle_t)，旧版 `i2c_bus_create()` 已废弃
- **显示**: SPI 屏用 `SpiLcdDisplay`，I2C OLED 用 `OledDisplay`，表情屏用 `emote::EmoteDisplay`
- **SPI 初始化**: 使用 `spi_bus_initialize(SPI2_HOST/SPI3_HOST, ...)` + `esp_lcd_new_panel_io_spi()`
- **背光**: 使用 `PwmBacklight` 类或自定义 Backlight 子类，支持空闲调暗（`kIdleBacklightDelayUs`）
- **按键**: 使用 `Button` 类（`boards/common/button.h`），支持 Click/LongPress/DoubleClick
- **分区表**: v2 版本使用 `partitions/v2/` 分区表，与 v1 不兼容

## 移植旧板型到 v2 的要点

1. **I2C API 变化**: 旧版 `i2c_bus_create()` → 新版 `i2c_new_master_bus()`
2. **Board 基类**: 新版 `WifiBoard` / `DualNetworkBoard` 可能接口有变化，对比 `wifi_board.h` / `dual_network_board.h`
3. **Display 构造**: `SpiLcdDisplay` 参数签名可能不同，查阅 `display/lcd_display.h`
4. **BoxAudioCodec**: 构造函数参数需要 `i2c_master_bus_handle_t` (void*) 而非旧版 i2c_bus_handle_t
5. **外部组件**: qmi8658 (IMU)、bq27220 (电量)、ws2812b (RGB灯) 等可能需要从旧项目复制或使用 managed_components

## 相关文档

- [自定义板型开发](docs/custom-board_zh.md)
- [MCP 协议](docs/mcp-protocol_zh.md)
- [WebSocket 通信](docs/websocket_zh.md)
- [MQTT+UDP 通信](docs/mqtt-udp_zh.md)
- [BLUFI 配网](docs/blufi_zh.md)
- [AetherToys WiFi-CN 开发文档](docs/AethToys/wifi-cn-development.md)
