# Unit LCD Plus 编码器与按键 I2C 协议

文档状态：开发者已确认

适用固件：Unit LCD Plus 0.5 及以上
默认 I2C 地址：`0x3E`

本文是编码器与按键专项说明。包含全部显示、读回、地址、复位和 IAP 命令的统一规范见
[`Unit_LCD_Plus_I2C_Protocol_zh.md`](Unit_LCD_Plus_I2C_Protocol_zh.md)。

## 1. 范围

本协议为 Unit LCD Plus 增加旋转编码器和按键输入能力，不改变原 Unit LCD
显示命令、默认 I2C 地址、地址持久化行为和在线升级命令。

- 编码器 A：GPIO2
- 编码器 B：GPIO3
- 编码器按键：GPIO4，低电平有效
- 编码器采样周期：1 ms
- 按键消抖时间：10 ms
- 双击判定窗口：300 ms，固定值
- 默认长按触发时间：500 ms
- 编码器仅上报机械步进，不实现旋转加速

按键支持 `PRESS`、`RELEASE`、`SINGLE_CLICK`、`DOUBLE_CLICK` 和
`LONG_PRESS`。长按触发时间可通过 I2C 读取和设置。

## 2. 传输约定

### 2.1 字节序与整数

- 所有多字节整数均使用大端序（Big Endian）。
- 编码器位置为有符号 32 位整数，达到 `INT32_MIN` 或 `INT32_MAX` 后饱和，
  不回绕。
- 旋转增量为有符号 16 位整数；正式固件每个机械步进只产生 `+1` 或 `-1`。
- 事件序号和丢弃计数为无符号 16 位整数。

### 2.2 I2C 事务

读命令使用以下事务：

```text
START + SLA(W) + command + REPEATED START + SLA(R) + response + STOP
```

也允许主机先写入命令并结束事务，再发起独立读事务。固件在收到读命令时锁存
本次响应；同一读事务中 TX FIFO 再填充不会切换到新的状态或下一条事件。

写命令使用以下事务：

```text
START + SLA(W) + command + parameters + STOP
```

除非命令定义中另有说明，写命令没有响应数据。主机应通过对应读命令确认写入
结果。请求长度不完整时不执行命令，也不修改已有状态。

## 3. 命令总表

| 命令 | 名称 | 请求长度 | 响应长度 | 功能 |
| --- | --- | ---: | ---: | --- |
| `0xB0` | `READ_ENCODER_STATE` | 1 | 8 | 读取当前位置、按键状态和队列统计 |
| `0xB1` | `PEEK_ENCODER_EVENT` | 1 | 10 | 查看队首事件，不删除 |
| `0xB2` | `ACK_ENCODER_EVENT` | 3 | 0 | 按序号确认并删除队首事件 |
| `0xB3` | `SET_ENCODER_POSITION` | 5 | 0 | 设置位置，并清空事件和溢出统计 |
| `0xB4` | `READ_BUTTON_CONFIG` | 1 | 2 | 读取长按触发时间 |
| `0xB5` | `SET_BUTTON_CONFIG` | 3 | 0 | 设置长按触发时间 |

命令 `0xB0` 至 `0xB5` 使用原协议未占用的编号。旧主机不发送这些命令时，
行为与原 Unit LCD 固件一致。

## 4. 状态读取

### 4.1 `0xB0 READ_ENCODER_STATE`

请求：

```text
Byte 0      0xB0
```

响应：

```text
Byte 0      flags
Byte 1      pending_count, uint8，范围 0..64
Byte 2..5   position, int32 BE
Byte 6..7   dropped_count, uint16 BE
```

`flags` 定义：

| 位 | 名称 | 含义 |
| ---: | --- | --- |
| bit0 | `BUTTON_PRESSED` | 当前物理按键已经消抖并处于按下状态 |
| bit1 | `BUTTON_LONG_PRESSED` | 当前这次按下已经触发长按 |
| bit2 | `EVENT_PENDING` | 事件队列非空 |
| bit3..6 | Reserved | 固定为 0 |
| bit7 | `QUEUE_OVERFLOW` | 自上次清除后至少丢弃过一条事件 |

状态响应是一个原子快照。`pending_count`、`position` 和 `dropped_count` 与
同一响应中的标志对应同一读取时刻。

## 5. 事件读取与确认

### 5.1 `0xB1 PEEK_ENCODER_EVENT`

请求：

```text
Byte 0      0xB1
```

响应：

```text
Byte 0      event_type
Byte 1      event_flags
Byte 2..3   sequence, uint16 BE
Byte 4..5   delta, int16 BE
Byte 6..9   position, int32 BE
```

事件类型：

| 值 | 名称 | 说明 |
| ---: | --- | --- |
| `0` | `NONE` | 当前队列为空 |
| `1` | `ROTATE` | 编码器产生一个机械步进 |
| `2` | `PRESS` | 按键消抖后确认按下 |
| `3` | `RELEASE` | 按键消抖后确认释放 |
| `4` | `SINGLE_CLICK` | 双击窗口到期后确认一次单击；兼容原草案的 `CLICK` 数值 |
| `5` | `LONG_PRESS` | 按住时间首次达到长按阈值 |
| `6` | `DOUBLE_CLICK` | 双击窗口内完成两次短按 |
| `7..255` | Reserved | 保留，主机不得依赖 |

`event_flags` 使用与状态响应相同的位定义，表示事件产生时的按键和队列状态。
非旋转事件的 `delta` 固定为 0。所有事件的 `position` 均为事件产生时的位置。

空队列响应：

```text
event_type = NONE
event_flags = 当前按键状态和溢出状态，EVENT_PENDING 为 0
sequence = 0xFFFF
delta = 0
position = 当前编码器位置
```

`PEEK_ENCODER_EVENT` 不删除事件。主机重复读取时得到相同的队首事件，直到使用
正确序号执行 ACK。

### 5.2 `0xB2 ACK_ENCODER_EVENT`

请求：

```text
Byte 0      0xB2
Byte 1..2   sequence, uint16 BE
```

仅当 `sequence` 等于当前队首事件序号时，固件才删除队首事件。以下情况均不
产生副作用：

- 队列为空；
- 序号错误；
- 重复 ACK；
- ACK 已经被队列溢出丢弃的旧事件；
- ACK 一个尚未成为队首的后续事件。

推荐的主机消费流程：

1. 发送 `PEEK_ENCODER_EVENT`。
2. 若类型为 `NONE`，停止本轮消费。
3. 完整处理并保存事件。
4. 使用响应中的 `sequence` 发送 `ACK_ENCODER_EVENT`。
5. 重新读取队首，直到类型为 `NONE`。

如果 I2C 读写失败，主机不得发送 ACK。重新连接后应重新 PEEK 当前队首。

## 6. 位置设置

### 6.1 `0xB3 SET_ENCODER_POSITION`

请求：

```text
Byte 0      0xB3
Byte 1..4   position, int32 BE
```

命令原子执行以下操作：

- 将当前位置设置为请求值；
- 清空事件队列；
- 取消已经释放、但仍在双击窗口内等待确认的单击候选；
- 将 `dropped_count` 清零；
- 清除 `QUEUE_OVERFLOW` 标志；
- 不重置下一事件序号；
- 不改变当前物理按键状态；
- 不改变长按时间配置。

命令不取消一个正在进行的按键动作。命令完成后发生的按键状态变化仍可生成
后续事件，事件中的 `position` 使用新位置。

## 7. 按键配置

### 7.1 `0xB4 READ_BUTTON_CONFIG`

请求：

```text
Byte 0      0xB4
```

响应：

```text
Byte 0..1   long_press_ms, uint16 BE
```

`long_press_ms` 是从消抖后的 `PRESS` 状态开始计时，到产生 `LONG_PRESS` 事件的
时间。计时精度受按键采样周期影响。

### 7.2 `0xB5 SET_BUTTON_CONFIG`

请求：

```text
Byte 0      0xB5
Byte 1..2   long_press_ms, uint16 BE
```

参数规则：

- 单位为毫秒；
- 有效范围为 `500..5000 ms`；
- 默认值为 `500 ms`；
- 超出范围的请求不生效，原配置保持不变；
- 配置只在当前运行周期有效，设备重启或软件复位后恢复默认值；
- 不写入 NVS，不影响 I2C 地址持久化数据；
- 命令不清空事件队列和溢出统计。

若设置命令发生在按键已经按下之后，本次按下继续使用 `PRESS` 时锁存的旧阈值，
新阈值从下一次 `PRESS` 开始生效。主机发送设置命令后应使用
`READ_BUTTON_CONFIG` 回读确认。

设置长按时间为 800 ms 的示例：

```text
Write: B5 03 20
Read command: B4
Expected response: 03 20
```

## 8. 按键事件判定

### 8.1 单击

一次按下在长按阈值前释放，并且释放后的 300 ms 双击窗口内没有第二次有效按下：

```text
PRESS -> RELEASE -> [等待双击窗口] -> SINGLE_CLICK
```

`PRESS` 和 `RELEASE` 实时进入队列。`SINGLE_CLICK` 必须等待双击窗口结束后才能
确定，因此相对 `RELEASE` 最多延迟约 300 ms。

### 8.2 双击

第一次短按释放后 300 ms 内开始第二次有效按下，并且第二次也在长按阈值前释放：

```text
PRESS -> RELEASE -> PRESS -> RELEASE -> DOUBLE_CLICK
```

双击只产生一个 `DOUBLE_CLICK`，不额外产生两个 `SINGLE_CLICK`。

### 8.3 长按

按住时间达到当前动作锁存的长按阈值：

```text
PRESS -> LONG_PRESS -> RELEASE
```

- `LONG_PRESS` 在达到阈值时产生一次，不等待释放。
- 继续按住不会周期性重复产生 `LONG_PRESS`。
- 长按释放后不产生 `SINGLE_CLICK` 或 `DOUBLE_CLICK`。
- `BUTTON_LONG_PRESSED` 在触发后保持为 1，直到按键释放。

### 8.4 第二次按下转为长按

如果双击候选动作的第二次按下持续到长按阈值，则本组动作按长按处理：

```text
PRESS -> RELEASE -> PRESS -> LONG_PRESS -> RELEASE
```

此时不产生 `SINGLE_CLICK` 或 `DOUBLE_CLICK`。

### 8.5 按住时旋转

旋转和按键共用一个事件队列，按实际产生顺序交错排列。例如：

```text
PRESS -> ROTATE(+1) -> ROTATE(+1) -> RELEASE -> SINGLE_CLICK
```

固件不合并连续旋转事件，也不因按键按下而改变旋转增量。

## 9. 事件队列

- 队列为固定内存环形队列，容量 64 条。
- 运行期间不为事件动态分配内存。
- 新事件入队时若队列已满，丢弃最旧事件并保留最新事件。
- 每丢弃一条事件，`dropped_count` 加 1，并饱和于 `65535`。
- `dropped_count != 0` 时，`QUEUE_OVERFLOW` 标志为 1。
- 只有 `SET_ENCODER_POSITION` 清空队列和溢出统计。
- 事件序号按 `uint16_t` 从 0 开始递增，在 `65535 -> 0` 处自然回绕。
- `SET_ENCODER_POSITION` 和 `SET_BUTTON_CONFIG` 均不重置事件序号。
- 设备重启后，事件序号、队列、位置和溢出统计从初始值开始。

队列溢出后，主机应把 `dropped_count` 作为输入丢失记录，并继续从当前队首逐条
PEEK/ACK。由于显式 ACK 只比较当前队首，即使序号回绕也不会删除错误位置的事件。

## 10. 典型数据示例

### 10.1 读取状态

```text
Write: B0
Read:  04 02 00 00 00 0C 00 00
```

解释：按键未按下，队列非空，待处理 2 条，位置为 12，没有事件丢失。

### 10.2 读取并确认旋转事件

```text
Write: B1
Read:  01 04 00 2A 00 01 00 00 00 0C
Write: B2 00 2A
```

解释：序号 42 的 `ROTATE` 事件，增量 `+1`，事件发生时位置为 12；最后一行确认
并删除该事件。

### 10.3 读取空队列

```text
Write: B1
Read:  00 00 FF FF 00 00 00 00 00 0C
```

解释：队列为空，序号为 `0xFFFF`，当前位置为 12。

### 10.4 设置负位置

```text
Write: B3 FF FF FF 9C
```

解释：位置设置为 -100，同时清空队列和溢出统计。

## 11. 主机实现要求

- 主机必须把 `position` 和 `delta` 按有符号整数解析。
- 主机只有在完整收到并处理事件后才发送 ACK。
- 主机不得通过“读取即删除”假设消费事件。
- 主机应接受事件序号回绕，不能使用简单的大于/小于关系判断新旧。
- 主机应忽略保留状态位和未知事件类型，并继续保持 I2C 通信。
- 主机暂停消费时可以低频读取状态，但不得 PEEK 后提前 ACK。
- 通信恢复后应保留本地主机的最后显示状态，并从设备当前队首继续消费。

## 12. 兼容性与版本

- 固件生产版本为 0.5；IAP 验证版本为 0.6。
- `READ_ID` 保持原 4 字节格式，仅固件次版本字段更新。
- 原显示、亮度、睡眠、旋转、RAW/RLE/Alpha、像素读取、地址设置和 IAP 命令不变。
- `SINGLE_CLICK=4` 与前一版草案中的 `CLICK=4` 二进制兼容，仅规范名称。
- 新主机在使用 `0xB4/0xB5` 前应通过 `READ_ID` 判断固件能力。

## 13. 已确认参数

开发者已确认以下协议参数：

1. 双击判定窗口固定为 300 ms。
2. 长按时间有效范围为 500..5000 ms。
3. 长按配置不掉电保存，重启恢复默认 500 ms。
4. 第二次按下转为长按时，整组动作不产生单击或双击事件。
