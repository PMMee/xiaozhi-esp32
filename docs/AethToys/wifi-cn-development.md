# AetherToys WiFi-CN 板开发文档

> **适用板型**: `Aethertoys-wifi-CN`（艾泽盒子 WiFi 中国版）  
> **更新日期**: 2026-07-01  
> **主控芯片**: ESP32-S3 (QFN56), 8MB PSRAM  
> **核心功能**: 闹钟管理 / 本地音乐播放 / 语音交互 / 双屏表情显示

---

## 目录

1. [硬件概述](#1-硬件概述)
2. [引脚配置一览](#2-引脚配置一览)
3. [闹钟系统](#3-闹钟系统)
   - [3.0 实现状态](#30-实现状态)
   - [3.1 架构设计](#31-架构设计)
   - [3.2 数据结构](#32-数据结构)
   - [3.3 核心流程](#33-核心流程)
   - [3.4 闹铃音乐策略](#34-闹铃音乐策略)
   - [3.5 持久化存储](#35-持久化存储)
   - [3.6 MCP 工具接口](#36-mcp-工具接口)
   - [3.7 使用示例](#37-使用示例)
4. [音乐播放系统](#4-音乐播放系统)
   - [4.1 Music 抽象接口](#41-music-抽象接口)
   - [4.2 播放流程](#42-播放流程)
   - [4.3 MCP 工具接口](#43-mcp-工具接口)
   - [4.4 默认播放列表](#44-默认播放列表)
   - [4.5 音乐源配置](#45-音乐源配置)
5. [设置存储系统 (NVS)](#5-设置存储系统-nvs)
6. [构建与烧录](#6-构建与烧录)
7. [调试与常见问题](#7-调试与常见问题)
8. [扩展开发指南](#8-扩展开发指南)

---

## 1. 硬件概述

WiFi-CN 是 AetherToys 项目的 **纯 WiFi 版本** 板型，不依赖 4G 模块即可独立工作。硬件特点：

| 组件 | 型号/规格 | 说明 |
|------|----------|------|
| 主控 | ESP32-S3 QFN56 | 双核 Xtensa LX7, 240MHz |
| 内存 | 8MB PSRAM | 支持大缓冲区音频处理 |
| 显示屏 | GC9D01 × 2 | 0.71" 圆形 LCD, 160×160 |
| 音频输入 | ES7210 MEMS麦克风 | 4通道ADC, I2S接口 |
| 音频输出 | ES8311 DAC + 功放 | I2S接口, PA使能 GPIO 48 |
| 姿态传感器 | QMI8658 | 6轴 IMU (加速度+陀螺) |
| 电量检测 | ADC_CHANNEL_2 (IO3) | 支持充电完成检测 (IO13) |
| 灯光 | WS2812B RGB | 单线控制, GPIO 15 |
| 触摸 | TOUCH 4/5 | 铜箔电容触摸 |
| 网络 | WiFi 2.4GHz | 纯WiFi, 无4G模块 |

**关键设计决策**:
- 采样率统一为 **24kHz**（`AUDIO_INPUT_SAMPLE_RATE` / `AUDIO_OUTPUT_SAMPLE_RATE`）
- 显示使用 SPI 80MHz 高速模式，独立左右屏片选
- 背光支持独立 PWM 控制，空闲自动调暗（30秒后降至 15%）
- 软件关机通过 GPIO 21 输出低电平触发

---

## 2. 引脚配置一览

> 配置文件: `main/boards/Aethertoys-wifi-CN/config.h`

### 音频接口 (I2S)

| 功能 | 引脚 | 宏定义 |
|------|------|--------|
| MCLK | GPIO 1 | `AUDIO_I2S_GPIO_MCLK` |
| WS (字选) | GPIO 44 | `AUDIO_I2S_GPIO_WS` |
| BCLK (位时钟) | GPIO 43 | `AUDIO_I2S_GPIO_BCLK` |
| DIN (麦克风输入) | GPIO 2 | `AUDIO_I2S_GPIO_DIN` |
| DOUT (扬声器输出) | GPIO 42 | `AUDIO_I2S_GPIO_DOUT` |
| PA 使能 | GPIO 48 | `AUDIO_CODEC_PA_PIN` |

### I2C 总线

| 功能 | 引脚 | 宏定义 |
|------|------|--------|
| SDA | GPIO 7 | `AUDIO_CODEC_I2C_SDA_PIN` |
| SCL | GPIO 6 | `AUDIO_CODEC_I2C_SCL_PIN` |
| ES8311 地址 | `ES8311_CODEC_DEFAULT_ADDR` | 默认 I2C 地址 |
| ES7210 地址 | `ES7210_CODEC_DEFAULT_ADDR` | 默认 I2C 地址 |

### 显示屏 (SPI)

| 功能 | 引脚 | 宏定义 |
|------|------|--------|
| SCLK | GPIO 16 | `DISPLAY_SPI_SCLK_PIN` |
| MOSI | GPIO 17 | `DISPLAY_SPI_MOSI_PIN` |
| DC (数据/命令) | GPIO 18 | `DISPLAY_SPI_DC_PIN` |
| RESET | GPIO 3 | `DISPLAY_SPI_RESET_PIN` |
| 左屏 CS | GPIO 8 | `DISPLAY_LEFT_SPI_CS_PIN` |
| 右屏 CS | GPIO 9 | `DISPLAY_RIGHT_SPI_CS_PIN` |
| 左屏背光 | GPIO 46 | `DISPLAY_LEFT_BACKLIGHT_PIN` |
| 右屏背光 | GPIO 10 | `DISPLAY_RIGHT_BACKLIGHT_PIN` |
| SPI 时钟 | 80 MHz | `LCD_SPI_PCLK_HZ` |

### 双屏旋转配置

```cpp
// 左屏：左转 90° (CCW)
DISPLAY_LEFT_SWAP_XY   = true
DISPLAY_LEFT_MIRROR_X  = false
DISPLAY_LEFT_MIRROR_Y  = true

// 右屏：右转 180°
DISPLAY_RIGHT_SWAP_XY  = false
DISPLAY_RIGHT_MIRROR_X = true
DISPLAY_RIGHT_MIRROR_Y = true
```

### 电源与电池

| 功能 | 引脚/通道 | 宏定义 |
|------|----------|--------|
| 电池电压 ADC | ADC_CHANNEL_2 (IO3) | `VBAT_ADC_CHANNEL` |
| 充电完成检测 | GPIO 13 (低有效) | `CHARGE_STDBY_PIN` |
| 满电电压 | 4100mV | `FULL_BATTERY_VOLTAGE` |
| 空电电压 | 3200mV | `EMPTY_BATTERY_VOLTAGE` |
| ADC 衰减 | 12dB | `ADC_ATTEN_DB_12` |

### 其他外设

| 功能 | 引脚 | 宏定义 |
|------|------|--------|
| BOOT 按键 | GPIO 0 | `BOOT_BUTTON_GPIO` |
| RGB 灯带 | GPIO 15 | `RGB_GPIO` |
| 触摸 1 | TOUCH_CHANNEL 4 | `TOUCH_1` |
| 触摸 2 | TOUCH_CHANNEL 5 | `TOUCH_2` |
| 4G 使能 | GPIO 14 (未使用) | `EN_4G` |
| 4G TX | GPIO 38 (未使用) | `ML307_TX_PIN` |
| 4G RX | GPIO 39 (未使用) | `ML307_RX_PIN` |
| 空闲关机时间 | 30000ms | `IDLE_SHUTDOWN_TIME` |

> **注意**: WiFi-CN 版本虽然定义了 4G 相关引脚，但实际不使用。网络类型固定为 WiFi。

---

## 3. 闹钟系统

### 3.0 实现状态

> ✅ **已实现 (2026-07-01)** — 从参考项目移植并优化，源代码位于:
> - `main/alarm_manager.h` — 闹钟管理器头文件
> - `main/alarm_manager.cc` — 闹钟管理器实现 (~500 行)
> - 资源文件: `main/assets/common/alarm.ogg`
>
> **优化点**（相比参考项目）:
> | 优化 | 说明 |
> |------|------|
> | FreeRTOS 音乐下载 | 替代 `std::thread`，符合 MCP 线程模型约定（参考 AGENTS.md） |
> | 纯 UTC 时间计算 | 替代 `localtime_r/mktime`，避免 DST 时区切换问题 |
> | `UpdateAlarmTime()` | 新增修改闹钟时间的方法（参考项目遗漏但文档定义） |
> | `mcp_server.cc` 注册 | 6 个 `self.alarm.*` MCP 工具在 `AddCommonTools()` 中注册，全板型通用 |
> | `application.cc` 启动 | `Initialize()` 中自动调用 `Start()`，无需板型单独接线 |
> | BOOT 按钮停止 | 单击按钮优先停止正在响铃的闹钟（最高优先级） |
> | **唤醒词打断** | 唤醒词检测时自动停止闹钟并清理音频管线再开始对话 |
>
> **注册位置**:
> | 文件 | 修改内容 |
> |------|---------|
> | `main/CMakeLists.txt` | 在 `"mcp_server.cc"` 后添加 `"alarm_manager.cc"` |
> | `main/mcp_server.cc` | `#include "alarm_manager.h"` + 在 `AddCommonTools()` 注册 6 个 `self.alarm.*` 工具 |
> | `main/application.cc` | `#include "alarm_manager.h"` + `Initialize()` 中 `Start()` + `Run()` 中唤醒词打断逻辑 |
> | `main/assets/lang_config.h` | 添加 `OGG_ALARM` 资源引用（`_binary_alarm_ogg_start`） |
> | `main/boards/aethertoys-wifi-cn/dual-screen.cc` | `#include "alarm_manager.h"` + `OnClick()` 中 `IsRinging()` 检查 |
>
> **全部 6 个 MCP 工具**:
> | 工具名 | 参数 | 功能 |
> |--------|------|------|
> | `self.alarm.set` | `time` (HH:MM), `repeat_daily`, `label` | 创建闹钟 |
> | `self.alarm.cancel` | `id` | 删除闹钟 |
> | `self.alarm.list` | 无 | 列出所有闹钟 |
> | `self.alarm.enable` | `id`, `enabled` | 启用/禁用 |
> | `self.alarm.update_time` | `id`, `time` | 修改时间 |
> | `self.alarm.set_music` | `song_name`, `artist_name` | 设置闹铃音乐 |
>
> **调试中发现的 Bug 与修复**:
>
> | # | 问题 | 根因 | 修复 |
> |---|------|------|------|
> | 1 | 日志输出"Alarm ringing for ld seconds" | ESP-IDF 的 `printf` 不完全支持 `%lld` 格式符 | 改用 `%d` + `(int)` 强转 |
> | 2 | 唤醒词"你好小安"无法打断闹钟 | 闹钟中断 AI 说话后，MQTT 音频通道残留"已打开"状态 → `HandleWakeWordDetectedEvent` 走错代码路径 → `ContinueWakeWordInvoke` 因 state!=Connecting 直接 return | 唤醒词打断闹钟时额外调用 `protocol_->CloseAudioChannel()` 清除残留状态，确保走正确的 `SetState(Connecting)→Schedule` 路径 |
> | 3 | 主循环 `vTaskDelay(500)` 阻塞导致 SCHEDULE 任务无法执行 | `vTaskDelay` 在事件循环内执行会阻止所有排队任务（含 `DismissAlert`）被处理 | 改为同步冲刷 `main_tasks_` 队列（`std::move` + 直接执行）替代延迟等待 |
>
> **Bug #2 详细分析**（最重要）:
> ```
> 闹钟中断 AI 说话 → SetDeviceState(Idle)
>   └─ 状态机切到 idle，但 MQTT 连接还活着，音频通道未正确关闭
>         │
> 用户说唤醒词 → HandleWakeWordDetectedEvent(state=idle)
>   └─ protocol_->IsAudioChannelOpened() → TRUE（脏状态！）
>         ├─ 走 ContinueWakeWordInvoke 直通路径（跳过 SetState(Connecting)）
>         └─ ContinueWakeWordInvoke: GetState() == Idle != Connecting → return
>               └─ 闹钟停了但设备永久卡在 idle，58s 后 MQTT 超时
>
> 修复（application.cc Run() 的 MAIN_EVENT_WAKE_WORD_DETECTED 分支）:
>   1. StopRinging()                     // 停止闹钟
>   2. flush main_tasks_                 // 同步执行 DismissAlert + music->Stop
>   3. protocol_->CloseAudioChannel()    // ← 关闭残留的半开通道
>   4. HandleWakeWordDetectedEvent()     // 现在 IsAudioChannelOpened()=false ✅
>      → SetState(Connecting) → Schedule(ContinueWakeWordInvoke)
>      → 下一次循环: OpenAudioChannel → 发唤醒词 → 服务器响应 ✅
> ```

### 3.1 架构设计

闹钟系统由 `AlarmManager` 单例管理，运行在 **ESP 定时器任务** 上下文中（`ESP_TIMER_TASK`）。

```
┌─────────────────────────────────────────────────────────┐
│                      Application                         │
│  ┌─────────────┐     ┌──────────────────────────────┐   │
│  │  McpServer   │────▶│       AlarmManager            │   │
│  │  (MCP工具)   │     │  ┌──────────┐ ┌───────────┐  │   │
│  │              │     │  │ timer_   │ │ring_timer_│  │   │
│  │ self.alarm.* │     │  │ (1s)     │ │ (1s)      │  │   │
│  └─────────────┘     │  └────┬─────┘ └─────┬─────┘  │   │
│                      │       │              │        │   │
│                      │  HandleTimer()  HandleRing() │   │
│                      │       │              │        │   │
│                      │       ▼              ▼        │   │
│                      │  StartRinging()  Alert/Dismiss│   │
│                      └──────────────────────────────┘   │
│                                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │              Settings (NVS: "alarm")              │   │
│  │  items (JSON) | music_song | music_artist         │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**两个定时器**:

| 定时器 | 周期 | 作用 |
|--------|------|------|
| `timer_` | 1 秒 | 检查是否有闹钟到达触发时间 |
| `ring_timer_` | 1 秒 | 管理响铃状态（超时停止 / 重复提醒） |

### 3.2 数据结构

```cpp
struct AlarmItem {
    std::string id;          // 唯一ID: "alarm_<timestamp_us>"
    bool enabled = true;     // 是否启用
    bool daily = true;       // true=每日重复, false=单次
    int hour = 0;            // 触发小时 (0-23)
    int minute = 0;          // 触发分钟 (0-59)
    std::string label;       // 标签（如"起床"、"开会"）
    time_t next_trigger = 0; // 下次触发时间戳 (UTC)
};
```

**闹铃音乐配置** (存储于 `AlarmManager` 内部):

```cpp
std::string alarm_music_song_;    // 歌曲名（空=使用本地 alarm.ogg）
std::string alarm_music_artist_;  // 艺术家名
```

### 3.3 核心流程

#### 3.3.1 启动流程

```
AlarmManager::Start()
  ├── 创建 timer_ (1s 周期, ESP_TIMER_TASK)
  ├── 创建 ring_timer_ (1s 周期, ESP_TIMER_TASK)
  ├── LoadFromSettings()       → 从 NVS 加载闹钟列表
  ├── LoadAlarmMusicLocked()   → 从 NVS 加载闹铃音乐配置
  ├── NormalizeLoadedAlarmsLocked() → 归一化（修正时间跳变后的触发时间）
  └── RearmTimerLocked()       → 无已启用闹钟则停止timer, 有则启动
```

#### 3.3.2 触发检测流程 (`HandleTimer`)

```
HandleTimer() [每1秒调用]
  │
  ├── 检测系统时间跳变 (>120秒)
  │   └── NormalizeLoadedAlarmsLocked()  重新计算所有闹钟的 next_trigger
  │
  ├── 遍历所有已启用闹钟
  │   └── next_trigger <= now + 1 ?
  │       ├── [每日闹钟] → 重新计算下一天触发时间
  │       └── [单次闹钟] → enabled = false, 稍后从列表删除
  │
  ├── 清理已禁用的单次闹钟
  ├── SaveToSettings() + RearmTimerLocked()
  │
  └── 对所有触发的闹钟调用 StartRinging()
```

#### 3.3.3 响铃流程 (`StartRinging` + `HandleRingTimer`)

```
StartRinging(item)
  │
  ├── 设置 ring_end_us_ = now + 60秒
  ├── 读取闹铃音乐配置 (song_name, artist_name)
  │
  ├── 判断播放策略:
  │   ├── 歌曲名/艺术家名为空 → 本地 alarm.ogg
  │   └── 有有效歌曲信息 → 在线音乐流式播放
  │
  └── 在 Application 主事件循环中:
      ├── [设备非空闲] → SwitchToIdle() 强制打断当前对话
      ├── StopStreaming() 清理残留音频流
      │
      ├── [本地模式] → Alert("Alarm", msg, "neutral", OGG_ALARM)
      │
      └── [音乐模式] → Alert("Alarm", msg, "neutral", "")
                       + 异步线程 Download(song, artist)
                       + 下载失败则回退到 OGG_ALARM

HandleRingTimer() [每1秒调用]
  │
  ├── 检查是否超时 (now >= ring_end_us_)
  │   └── [超时] → 停止 ring_timer_, DismissAlert(), StopStreaming()
  │
  └── [未超时] → 每2秒重新发送 Alert (kRingAlertIntervalUs)
      ├── [本地模式] → 重复播放 alarm.ogg
      └── [音乐模式] → 仅刷新 Alert 消息（音乐持续播放）
```

**关键常量**:

| 常量 | 值 | 说明 |
|------|-----|------|
| `kRingDurationUs` | 60 秒 | 响铃总时长 |
| `kRingIntervalUs` | 1 秒 | ring_timer 检查间隔 |
| `kRingAlertIntervalUs` | 2 秒 | Alert 重复发送间隔（让屏幕刷新） |
| `kMinTimerUs` | 1 秒 | timer_ 检查间隔 |

#### 3.3.4 停止响铃 (`StopRinging`)

```cpp
bool AlarmManager::StopRinging() {
    // 1. 清除响铃状态 (ringing_ = false)
    // 2. 停止 ring_timer_
    // 3. 通过 Application::Schedule() 在主事件循环中:
    //    - music->StopStreaming() 停止音乐
    //    - app.DismissAlert() 关闭闹钟弹窗
}
```

#### 3.3.5 时间跳变处理

当系统时间发生大于 120 秒的跳变时（如 NTP 同步后），`HandleTimer` 会检测到并自动调用 `NormalizeLoadedAlarmsLocked()`：

- 重新计算所有已启用闹钟的 `next_trigger`
- 单次闹钟如果触发时间已过 → 自动禁用并删除
- 保存更新后的闹钟列表到 NVS

### 3.4 闹铃音乐策略

闹铃音乐支持两种模式：**本地铃音** 和 **在线音乐**。

```
判断逻辑 (ShouldUseMusicPlayback):
  ┌─────────────────────────────────────────┐
  │ song_name 为空 && artist_name 为空?      │
  │  → YES: 使用本地 alarm.ogg               │
  │  → NO:  继续判断                         │
  │                                         │
  │ song_name == "alarm.ogg" && artist 为空? │
  │  → YES: 使用本地 alarm.ogg (兼容旧版)     │
  │  → NO:  使用在线音乐播放                  │
  └─────────────────────────────────────────┘
```

**默认铃音关键词识别** (`IsDefaultAlarmMusicKeyword`):

以下关键词会被识别为"使用默认铃音"（大小写不敏感）:

| 中文关键词 | 英文/其他 |
|-----------|----------|
| 默认铃音 | `alarm.ogg` |
| 默认铃声 | `default` |
| 默认闹铃 | |
| 系统铃音 | |
| 系统铃声 | |
| 本地铃音 | |
| 本地铃声 | |

**兼容性迁移**: `SetAlarmMusic` 会自动检测旧版本遗留配置并迁移:
- `"alarm.ogg"` → 清空（使用本地铃音）
- `"月亮代表我的心"/"邓丽君"` → 清空（使用本地铃音）

### 3.5 持久化存储

闹钟数据存储在 **NVS (Non-Volatile Storage)** 的 `"alarm"` 命名空间下：

| 键 | 类型 | 格式 | 示例 |
|----|------|------|------|
| `items` | String (JSON) | 闹钟列表 | `{"alarms":[{...}]}` |
| `music_song` | String | 歌曲名 | `"稻香"` |
| `music_artist` | String | 艺术家名 | `"周杰伦"` |

**JSON 格式** (`items` 键值):

```json
{
  "alarms": [
    {
      "id": "alarm_1717400000123456",
      "enabled": true,
      "daily": true,
      "hour": 7,
      "minute": 30,
      "label": "起床",
      "next_trigger": 1717486200
    }
  ]
}
```

### 3.6 MCP 工具接口

闹钟功能通过 MCP (Model Context Protocol) 暴露给云端 AI，支持语音控制：

| MCP 工具名 | 功能 | 参数 |
|-----------|------|------|
| `self.alarm.set` | 创建新闹钟 | `time` (HH:MM), `repeat_daily` (bool), `label` (string) |
| `self.alarm.update_time` | 修改闹钟时间 | `id`, `time` (HH:MM) |
| `self.alarm.list` | 列出所有闹钟 | 无 |
| `self.alarm.cancel` | 删除闹钟 | `id` |
| `self.alarm.enable` | 启用/禁用闹钟 | `id`, `enabled` (bool) |
| `self.alarm.set_music` | 设置闹铃音乐 | `song_name`, `artist_name` |

**返回格式**:

```json
// 成功创建
{"success": true, "id": "alarm_1717400000123456"}

// 失败 (格式错误)
{"success": false, "error": "Invalid time format, expected HH:MM"}

// 失败 (单次闹钟时间已过)
{"success": false, "error": "One-shot alarm time has already passed"}

// 失败 (闹钟不存在)
{"success": false, "error": "alarm not found"}

// 列出闹钟
{
  "alarms": [
    {
      "id": "alarm_...",
      "enabled": true,
      "daily": true,
      "hour": 7,
      "minute": 30,
      "label": "起床",
      "next_trigger": 1717486200
    }
  ]
}
```

### 3.7 使用示例

#### 通过 C++ 代码操作闹钟

```cpp
#include "alarm_manager.h"

// 创建每日闹钟
std::string id, error;
bool ok = AlarmManager::GetInstance().AddAlarm("07:30", true, "起床", id, error);
if (!ok) {
    ESP_LOGE(TAG, "Failed: %s", error.c_str());
}

// 创建单次闹钟
AlarmManager::GetInstance().AddAlarm("14:00", false, "开会提醒", id, error);

// 修改闹钟时间
AlarmManager::GetInstance().UpdateAlarmTime(id, "08:00");

// 设置闹铃音乐
AlarmManager::GetInstance().SetAlarmMusic("晴天", "周杰伦", error);

// 恢复默认铃音
AlarmManager::GetInstance().SetAlarmMusic("", "", error);

// 删除闹钟
AlarmManager::GetInstance().CancelAlarm(id);

// 停止正在响铃的闹钟
AlarmManager::GetInstance().StopRinging();
```

#### 通过语音控制（MCP 工具）

用户语音 → 云端 AI → MCP 调用:
- "帮我设一个早上7点的闹钟" → `self.alarm.set` (time="07:00", repeat_daily=true)
- "把闹钟铃声换成稻香" → `self.alarm.set_music` (song_name="稻香", artist_name="周杰伦")
- "关闭所有闹钟" → `self.alarm.list` → 逐一 `self.alarm.cancel`
- "还有哪些闹钟" → `self.alarm.list`

#### 停止闹钟的四种方式

| 方式 | 触发者 | 实现位置 |
|------|--------|---------|
| ⏱ **60s 超时自动停** | `ring_timer_` 到期 | `alarm_manager.cc:HandleRingTimer()` → `DismissAlert()` + `Stop()` |
| 🔘 **BOOT 按钮单击** | 用户物理按键 | `dual-screen.cc:OnClick()` 中 `IsRinging()` 检查优先级最高 |
| 🗣 **唤醒词打断** | 语音唤醒词 | `application.cc:Run()` 中 `MAIN_EVENT_WAKE_WORD_DETECTED` 分支，停止闹钟 + 关闭残留音频通道 + 刷新 SCHEDULE 队列 |
| 💬 **语音指令** | AI 调用 MCP | 云端理解"关闭闹钟"→ 调用 `self.alarm.cancel` 删除闹钟 |

**BOOT 按钮优先级**（`dual-screen.cc` `InitializeButtons()`）:
```cpp
boot_button_.OnClick([this]() {
    // 1. 闹钟响铃中 → 停止闹钟（最高优先级）
    if (AlarmManager::GetInstance().IsRinging()) {
        AlarmManager::GetInstance().StopRinging();
        return;
    }
    // 2. 音乐播放中 → 停止音乐
    if (music_ && music_->IsPlaying()) { music_->Stop(); return; }
    // 3. 启动中 → 进入配网模式
    if (app.GetDeviceState() == kDeviceStateStarting) { ... }
    // 4. 正常 → 唤醒对话
    app.WakeWordInvoke(...);
});
```

**唤醒词打断闹钟流程**（`application.cc` `Run()` 中 `MAIN_EVENT_WAKE_WORD_DETECTED` 分支）:
```cpp
if (AlarmManager::GetInstance().IsRinging()) {
    // 1. 停止闹钟（设置 ringing_=false, 停 ring_timer_, Schedule DismissAlert+Stop）
    AlarmManager::GetInstance().StopRinging();
    // 2. 同步执行已排队的清理任务（DismissAlert + music->Stop）
    //    注意：不能用 vTaskDelay 阻塞主循环，否则 SCHEDULE 任务永远不会执行
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto pending = std::move(main_tasks_);
        lock.unlock();
        for (auto& task : pending) { task(); }
    }
    // 3. 关闭闹钟中断 AI 说话后残留的半开音频通道
    //    如果不做这一步，protocol_->IsAudioChannelOpened() 返回 true
    //    → HandleWakeWordDetectedEvent 走错分支 → 设备永久卡在 idle
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
}
HandleWakeWordDetectedEvent();  // 正常唤醒词流程
```

#### 闹钟触发时的音频抢占流程

当闹钟触发时，无论设备当前处于什么状态（Speaking/Listening/Idle），`AlarmManager` 会：
1. `SetDeviceState(kDeviceStateIdle)` — 强制切回空闲态，断开 WebSocket 语音通道
2. `music->Stop()` — 清空 I2S 输出缓冲区的残留 TTS/音乐数据
3. `Alert("Alarm", msg, "neutral", OGG_ALARM)` — 独占播放闹钟铃音
4. `ring_timer_` 启动 — 每 2s 重复 Alert（刷新屏幕显示 + 重复铃音）

---

## 4. 音乐播放系统

### 4.1 Music 抽象接口

`Music` 是一个纯虚基类（定义于 `main/boards/common/music.h`），具体实现在各板型的 Board 子类中。

```cpp
class Music {
public:
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,  // 频谱显示
        DISPLAY_MODE_LYRICS = 1     // 歌词显示
    };

    // 核心播放接口
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool PauseStreaming() = 0;
    virtual bool ResumeStreaming() = 0;
    virtual bool StopStreaming() = 0;

    // 状态查询
    virtual bool MusicPlaying() = 0;
    virtual bool IsPaused() const = 0;
    virtual bool HasActiveStream() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual size_t GetBufferSize() const = 0;

    // 音频数据获取（被 AudioOutputTask 循环调用）
    virtual int16_t* GetAudioData() = 0;

    // 本地文件播放
    virtual bool PlayLocalFile(const std::string& file_path) = 0;

    // 搜索（不播放，返回JSON）
    virtual std::string SearchMusic(const std::string& song_name, const std::string& artist_name);

    // 元数据
    virtual int GetTotalDuration() const = 0;
    virtual std::string Get_song_name() = 0;
    virtual std::string Get_artist_name() = 0;
    virtual DisplayMode GetDisplayMode() const = 0;

    // 音频管线集成
    virtual void ResetSampleRate() = 0;
};
```

### 4.2 播放流程

```
用户请求播放
     │
     ▼
Music::Download(song_name, artist_name)
     │
     ├── 向云端 API 发送搜索请求
     ├── 获取歌曲流媒体 URL
     └── 自动调用 StartStreaming(url)
           │
           ├── 创建 HTTP 流式下载任务
           ├── 解码器初始化（MP3/AAC→PCM）
           └── 填充 PCM 缓冲区
                 │
                 ▼
         AudioOutputTask 循环:
           while (music->MusicPlaying()) {
               int16_t* pcm = music->GetAudioData();
               audio_codec->Write(pcm, len);
           }
```

**与音频管线的集成**:
- 音乐播放时，`AudioOutputTask` 会优先从 `Music` 对象获取 PCM 数据
- 音乐 PCM 与 TTS 语音共享同一输出通路（I2S → ES8311 → 功放）
- 播放音乐前必须确保设备处于 `Idle` 状态（对话中会先 `SwitchToIdle()`）

### 4.3 MCP 工具接口

| MCP 工具名 | 功能 | 参数 |
|-----------|------|------|
| `self.music.play_music` | 搜索并播放音乐 | `song_name`, `artist_name` (均可选) |
| `self.music.search_music` | 仅搜索不播放 | `song_name`, `artist_name` |
| `self.music.stop_music` | 停止播放 | 无 |
| `self.music.resume_music` | 恢复已暂停的音乐 | 无 |
| `self.music.pause_music` | 暂停音乐 | 无 |

**`self.music.play_music` 特殊行为**:
- 如果 `song_name` 和 `artist_name` 都为空 → 从默认播放列表随机选择一首
- 如果设备正在说话 → 音乐排队，等待 TTS 播放完毕自动开始
- 播放前自动 `StopStreaming()` 清理之前的音频流

**`self.music.search` 异步执行与超时保护**:

> 实现：`main/boards/common/esp32_music.cc` 的 `Esp32Music::Search()` / `SearchTaskFunc()`

- **为何不能同步阻塞**：所有 MCP 工具回调都通过 `Application::Schedule()` 在**主事件循环**中执行（见 `mcp_server.cc` 的 `DoToolCall`）。主循环同时负责处理 `MAIN_EVENT_WAKE_WORD_DETECTED` 等事件位。若搜索同步做 HTTP 往返，一旦 TCP 连接卡死，唤醒词事件无法被处理 → 设备"叫不答应"。
- **`self.music.play` 不受影响**：`Start()` 只创建 FreeRTOS 任务后立即返回，搜索/下载/解码/播放全部在任务里异步跑。
- **搜索的超时机制**：
  - `Search()` 创建独立任务 `music_search`（栈 8KB，优先级 5）执行 `SearchMusicInternal()`
  - 主循环用 `xSemaphoreTake(done, 10s)` 等待结果；**超时立即返回** `{"success":false,"message":"搜索超时，请稍后再试"}`，主循环释放，唤醒词恢复响应
  - 超时时主循环会调 `active_http_->Close()` 中断卡死的 HTTP 连接，帮助搜索任务尽快退出
  - HTTP 自身 `SetTimeout(8000)`（`ReadAll` 受此约束，但 `Open` 的底层 `tcp_->Connect()` **不受** `SetTimeout` 约束，这是 10s 硬超时的兜底必要原因）
- **并发保护**：`search_task_running_` 原子标志防止并发搜索；若上一个搜索未结束，直接返回"正在搜索中，请稍候"
- **资源回收**：搜索任务在 `xSemaphoreGive` 后最多等 500ms 主线程取结果，然后自行 `delete` arg 并 `vTaskDelete(nullptr)`
- **已知局限**：若 `tcp_->Connect()` 真的永久阻塞且 `Close()` 无法中断，搜索任务栈（8KB）会泄漏，但主循环已不被阻塞，设备仍可响应唤醒词。彻底修需要让 `HttpClient::Open` 的 connect 也受 timeout 约束（属 `managed_components/78__esp-ml307` 范畴）

**搜索返回格式**:

```json
{
  "success": true,
  "results": [
    {
      "name": "晴天",
      "singer": "周杰伦",
      "url": "https://...",
      "pic": "https://...",
      "duration": 269
    }
  ]
}
```

### 4.4 默认播放列表

当用户未指定歌曲名时（如"放首歌"），系统从以下列表随机选择：

```cpp
static const PlaylistEntry kDefaultPlaylist[] = {
    {"稻香", "周杰伦"},
    {"晴天", "周杰伦"},
    {"夜曲", "周杰伦"},
    {"平凡之路", "朴树"},
    {"起风了", "买辣椒也用券"},
    {"光年之外", "邓紫棋"},
    {"演员", "薛之谦"},
    {"后来", "刘若英"},
    {"小幸运", "田馥甄"},
    {"追梦赤子心", "GALA"},
};
```

> 修改默认播放列表: 编辑 `main/mcp_server.cc` 中的 `kDefaultPlaylist` 数组。

### 4.5 音乐源配置

> **默认**: `kw`（酷我音乐）— 经实测 CDN 对 ESP32 最友好，稳定 1Mbps+
> **配置位置**: `main/boards/common/esp32_music.cc` 第 32 行 `MUSIC_SOURCE_TYPE`

音乐搜索通过 `shybot.top` 聚合 API，支持切换不同音乐平台：

| 值 | 平台 | ESP32 CDN 实测速度 | 流畅度 |
|----|------|-------------------|:---:|
| `kw` | 酷我音乐 | **~1 Mbps（129 KB/s）** | ✅ 流畅 |
| `qq` | QQ音乐 | ~68 Kbps（8.5 KB/s） | ❌ 卡顿 |
| `wy` | 网易云音乐 | 未测试 | — |
| `kg` | 酷狗音乐 | 未测试 | — |

**切换方法**：修改 `main/boards/common/esp32_music.cc` 顶部：

```cpp
#define MUSIC_SOURCE_TYPE "kw"   // 默认酷我，可选: qq | wy | kg | kw
```

**为什么 QQ 音乐慢**：QQ 音乐 CDN（`sjy6.stream.qqmusic.qq.com`）对 ESP32 客户端有明显限速，实测仅 8 KB/s，远低于 128kbps MP3 所需的 16 KB/s。酷我 CDN（`kw-er.kuwo.cn`）无此限制，稳定供速。

**音乐播放结束后的网络清理**：HTTP 流式播放经过多次超时/重试后，lwIP 协议栈可能残留 TCP 半开连接，导致后续 MQTT 重连 DNS 解析失败。`MusicTaskFunc` 出口已加入 2 秒延迟等待 TCP 释放后重连。

---

## 5. 设置存储系统 (NVS)

### Settings 类

```cpp
// 文件: main/settings.h

class Settings {
public:
    // ns: 命名空间 (用于隔离不同模块的数据)
    // read_write: true=可读写, false=只读
    Settings(const std::string& ns, bool read_write = false);

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();
};
```

**使用示例**:

```cpp
// 读取设置（只读）
Settings settings("alarm", false);
std::string json = settings.GetString("items", "");

// 写入设置（可读写）
Settings settings("alarm", true);
settings.SetString("music_song", "稻香");
settings.SetString("music_artist", "周杰伦");
settings.SetInt("volume", 80);
```

**各模块使用的命名空间**:

| 命名空间 | 用途 | 主要键 |
|---------|------|-------|
| `"alarm"` | 闹钟数据 | `items`, `music_song`, `music_artist` |
| `"settings"` | 系统设置 | 音量、亮度、网络配置等 |

---

## 6. 构建与烧录

### 环境要求

- **ESP-IDF**: v5.3+ （推荐使用 VS Code ESP-IDF 扩展）
- **工具链**: xtensa-esp32s3-elf-gcc
- **Python**: 3.8+

### 构建命令

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置项目（选择板型等）
idf.py menuconfig

# 构建
idf.py build

# 烧录（通过 USB-UART）
idf.py -p COM3 flash

# 烧录 + 打开串口监视器
idf.py -p COM3 flash monitor
```

### 在 VS Code 中构建

1. 安装 "ESP-IDF" 扩展
2. 打开命令面板 (`Ctrl+Shift+P`) → `ESP-IDF: Build your project`
3. 烧录: `ESP-IDF: Flash your project`
4. 监视: `ESP-IDF: Monitor your project`

### 分区表

分区配置在 `partitions/` 目录下，通常包含:
- `factory` — 主固件
- `ota_0` / `ota_1` — OTA 双分区
- `nvs` — 非易失存储（闹钟数据、设置等）
- `assets` — 资源分区（表情图片、音频文件）

---

## 7. 调试与常见问题

### 日志标签

| 标签 | 模块 | 日志级别建议 |
|------|------|-------------|
| `AlarmManager` | 闹钟管理 | INFO |
| `Application` | 应用状态机 | INFO/DEBUG |
| `McpServer` | MCP 服务器 | INFO |

### 常见问题

#### Q1: 闹钟不响

**排查步骤**:
1. 确认系统时间已同步（NTP 同步成功）
   ```
   查看日志: "System time changed significantly"
   ```
2. 检查闹钟是否启用: 调用 `self.alarm.list` 查看
3. 检查 `next_trigger` 是否在未来时间
4. 如果出现过时间跳变日志，`NormalizeLoadedAlarmsLocked()` 已自动修正

#### Q2: 闹铃音乐播放失败

**原因**: 网络不可用或云端 API 不可达

**回退机制**: 自动回退到本地 `alarm.ogg`（打包在固件中）

**日志特征**:
```
AlarmManager: Failed to start alarm music, fallback to alarm.ogg
```

#### Q3: 闹钟在对话中不打断

**预期行为**: `StartRinging` 会强制调用 `SetDeviceState(kDeviceStateIdle)` 打断当前对话，抢占 I2S 音频资源。

#### Q4: 单次闹钟重启后消失

**预期行为**: 单次闹钟在触发后或重启后如果时间已过会自动清除。这是设计如此。

#### Q5: 音乐播放与 TTS 声音冲突

**现象**: 音乐播放时 TTS 声音被覆盖或混叠

**原因**: 音乐 PCM 和 TTS PCM 共享同一输出通路（I2S → ES8311）

**解决**: 播放音乐前确保 `SetDeviceState(kDeviceStateIdle)` 完成，`music->Stop()` 清理缓冲区。

#### Q6: 搜索音乐时唤醒词无响应（卡在 self.music.search）

**现象**: AI 调用 `self.music.search` 后，设备卡死，唤醒词叫不应，需重启

**根因**: MCP 工具回调跑在主事件循环（`app.Schedule`），同步搜索的 HTTP 往返阻塞主循环 → `MAIN_EVENT_WAKE_WORD_DETECTED` 无法处理。`HttpClient::Open()` 的 `tcp_->Connect()` 不受 `SetTimeout` 约束，DNS/TCP 握手可无限阻塞。

**修复**: `Esp32Music::Search()` 改为在独立 FreeRTOS 任务 `music_search` 中执行 HTTP 搜索，主循环用 `xSemaphoreTake(done, 10s)` 带硬超时等待。超时立即返回"搜索超时"并调 `active_http_->Close()` 中断连接，主循环释放，唤醒词恢复。详见 §4.3。

**日志特征**:
```
[SEARCH] Timed out after 10000ms, aborting HTTP
```

#### Q7: 唤醒词"你好小安"无法打断闹钟（已修复）

**现象**: 闹钟响铃时说唤醒词，闹钟停止但设备无响应，~58秒后自动断开会话。

**日志特征**:
```
AlarmManager: Alarm ringing stopped
Application: Wake word detected: nihaoxiaoan (state: 3)
AfeWakeWord: Encode wake word opus 35 packets in 251 ms
[此后无任何状态转换，~58秒后 MQTT goodbye]
```

**根因**: 闹钟中断 AI 说话后 `SetDeviceState(Idle)` 只改了状态机状态，MQTT 音频通道仍处于"已打开"的脏状态。后续唤醒词触发 `HandleWakeWordDetectedEvent` 时，`protocol_->IsAudioChannelOpened()` 返回 true → 走了 `ContinueWakeWordInvoke` 直通路径（跳过 `SetState(Connecting)`）→ `ContinueWakeWordInvoke` 检查 state != Connecting → return，设备永久卡在 idle。

**修复**: 在 `application.cc` 的 `MAIN_EVENT_WAKE_WORD_DETECTED` 分支中，停止闹钟后额外调用 `protocol_->CloseAudioChannel()` 清除残留状态。详见 §3.0 的 Bug #2 分析。

#### Q8: 闹钟日志显示 "ringing for ld seconds"

**现象**: 串口日志输出 `Alarm ringing for ld seconds` 而非 `Alarm ringing for 60 seconds`

**根因**: ESP-IDF 的 `newlib` printf 不完全支持 `%lld` 格式符，`%ll` 被解析为 `%l`（无效）后原样输出 `l` + 字面量 `d`。

**修复**: 改用 `%d` + `(int)` 强转（`alarm_manager.cc`）。

### 调试技巧

```cpp
// 查看当前所有闹钟
cJSON* json = AlarmManager::GetInstance().GetAlarmsJson();
char* str = cJSON_Print(json);
ESP_LOGI(TAG, "Alarms: %s", str);
cJSON_free(str);
cJSON_Delete(json);

// 查看闹铃音乐配置
cJSON* music = AlarmManager::GetInstance().GetAlarmMusicJson();
// ...

// 检查响铃状态
if (AlarmManager::GetInstance().IsRinging()) {
    ESP_LOGI(TAG, "Alarm is currently ringing");
}
```

---

## 8. 扩展开发指南

### 8.1 添加新的闹钟功能

1. **修改 `alarm_manager.h`**: 在 `AlarmItem` 中添加新字段
2. **更新 `SaveToSettings()` / `LoadFromSettings()`**: 确保新字段被序列化
3. **添加 MCP 工具**: 在 `mcp_server.cc` 中注册新工具
4. **更新 JSON 序列化**: 在 `GetAlarmsJson()` 中添加新字段

### 8.2 自定义闹铃行为

修改 `StartRinging()` 方法可在闹钟触发时执行自定义逻辑:

```cpp
void AlarmManager::StartRinging(const AlarmItem& item) {
    // 现有逻辑...

    // 自定义：触发 LED 闪烁
    Application::GetInstance().Schedule([]() {
        Board::GetInstance().GetLed()->StartAnimation(/* ... */);
    });
}
```

### 8.3 添加新的闹铃音效

1. 将 OGG 文件放入 `main/assets/` 目录
2. 在 `CMakeLists.txt` 中添加嵌入资源声明
3. 在 `lang_config.h` 中声明对应的 `std::string_view`
4. 修改 `ShouldUseMusicPlayback()` 或 `StartRinging()` 中的判断逻辑

### 8.4 修改默认播放列表

编辑 `main/mcp_server.cc` 中的 `kDefaultPlaylist`:

```cpp
static const PlaylistEntry kDefaultPlaylist[] = {
    {"你的歌曲", "歌手名"},
    // ... 添加更多
};
```

---

## 相关文档

- [板级说明](board-cyberai-toy-lily-4G-2.md) — 硬件详细说明
- [MCP 协议](mcp-protocol.md) — Model Context Protocol 实现
- [MCP 使用指南](mcp-usage.md) — MCP 工具使用说明
- [WebSocket 通信](websocket.md) — WebSocket 协议实现
- [MQTT/UDP 通信](mqtt-udp.md) — MQTT+UDP 双通道实现
- [项目 README](../README.md) — 项目总览

---

*文档维护: 如有更新请同步修改本文件。*
