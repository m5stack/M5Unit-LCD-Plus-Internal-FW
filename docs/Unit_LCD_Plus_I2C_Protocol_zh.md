# Unit LCD Plus 完整 I2C 通信协议

| 项目 | 内容 |
| --- | --- |
| 文档版本 | 1.3 |
| 适用产品 | Unit LCD Plus（ESP32-C61） |
| 适用正式固件版本 | 1.2 |
| 默认 7 位 I2C 地址 | `0x3E` |

## 1. 文档范围

本文是 Unit LCD Plus 固件版本 1.2 的外部主控协议规范，统一描述：

- 设备识别和命令缓冲区查询；
- 显示反色、亮度、旋转、休眠和功耗控制；
- 显示区域、颜色、像素、矩形、RAW、RLE、Alpha 和区域复制；
- 显示像素读回；
- I2C 地址修改和软件复位；
- 编码器、按键状态、单击、双击、长按和事件队列；
- I2C 在线固件升级（IAP）。

本文以当前命令解析器和公共协议头为准；历史 Basic 主机实测记录对应其记录的旧版本，
不代表新增指令已经完成硬件验证。旧版 Unit LCD 的显示命令保持二进制兼容；
Unit LCD Plus 提供 `0xB0` 至 `0xB5` 输入命令，版本 1.2 新增 `0x0A READ_MAC`。

### 1.1 目录

1. 文档范围
2. 硬件与总线
3. I2C 事务规则
4. 数据表示
5. 命令总表
6. 设备信息与流量控制
7. 显示控制
8. 显示窗口与区域复制
9. 当前绘图色与基本绘图
10. RAW 像素流
11. RLE 像素流
12. 像素读回
13. 地址修改与软件复位
14. 编码器与按键
15. I2C 在线固件升级（IAP）
16. 主机实现检查清单
17. 版本兼容性
18. 实现依据与变更记录

### 1.2 规范性用语

本文中的“必须”“不得”表示实现协议兼容所需的强制要求；“应”“建议”表示正常主机
实现应遵循的推荐行为；“可以”表示可选行为。十六进制报文中的空格仅用于分隔字节，
不在线上传输。

## 2. 硬件与总线

| 项目 | 定义 |
| --- | --- |
| I2C 模式 | Unit LCD Plus 为从机，外部主控为主机 |
| 默认地址 | `0x3E`，7 位地址，不包含 R/W 位 |
| 地址范围 | `0x08..0x77` |
| 最高频率 | 400 kHz |
| Unit LCD Plus SDA | GPIO5 |
| Unit LCD Plus SCL | GPIO6 |
| 显示逻辑分辨率 | 135 x 240 像素，旋转后宽高相应交换 |
| 编码器 A/B | GPIO2 / GPIO3 |
| 编码器按键 | GPIO9，低电平有效 |

GPIO9 同时是 ESP32-C61 的下载启动绑带脚。上电或复位时按住编码器按键（拉低 GPIO9）
可进入 Joint Download Boot，用于 USB/UART 烧录。

主机必须与 Unit LCD Plus 共地。本文中的地址均为 7 位地址；部分 HAL 要求传入
`0x3E << 1` 时，应遵循该 HAL 的 API 定义，不能把 8 位地址直接写入本协议报文。

## 3. I2C 事务规则

### 3.1 固定长度写命令

```text
START + SLA(W) + command + parameters + STOP
```

- 一个固定长度命令必须在同一写事务中完整发送。
- 命令未收齐前出现 STOP 或重复 START，该命令不执行。
- 一个固定长度命令完成后，可在同一写事务中继续发送下一个固定长度命令。
- 除本文明确给出响应的命令外，写命令没有返回值。

### 3.2 不定长度写命令

`WRITE_RAW_*`、`WRITE_RLE_*` 和 `UPDATE_DATA` 包含不定长度数据：

- 命令头和数据必须处于同一个连续写事务；
- STOP 表示本次数据流结束；
- 不得在一个未结束的不定长度命令后直接拼接其他命令；
- 像素数据必须按完整像素或完整 RLE 单元结束；
- IAP 数据长度由 `UPDATE_BEGIN` 中的固件总长度和每块最大 4096 字节共同确定。

### 3.3 读命令

推荐使用组合事务：

```text
START + SLA(W) + read_command
REPEATED START + SLA(R) + response + STOP
```

也允许先用 STOP 结束命令写入，再发起独立读事务：

```text
START + SLA(W) + read_command + STOP
START + SLA(R) + response + STOP
```

编码器状态、事件和按键配置在收到读命令时锁存。显示像素读回在读事务中按当前读
窗口连续输出。主机应读取命令定义的完整响应长度。

### 3.4 命令缓冲与异步显示

I2C 接收和显示绘制异步执行。写事务成功只表示命令已被接收，不表示屏幕已经完成刷新。
主机应使用 `READ_BUFCOUNT (0x09)` 进行流量控制：

- 返回值越大，命令队列余量越多；
- `255` 表示队列基本空闲；
- `1` 表示当前几乎无余量，或设备正在执行地址持久化等独占操作；
- 该值是队列余量指标，不是可发送的字节数。

在写入像素后立即读回时，应等待队列恢复到较高余量，再额外留出显示任务处理时间。
官方验证程序使用 `READ_BUFCOUNT >= 128` 并在像素读回前等待 50 ms。

### 3.5 未定义命令和异常报文

- `0x00 NOP` 和未定义命令会忽略本事务剩余字节，直到 STOP。
- 参数不足的固定长度命令不执行。
- 对超出本文约束的坐标、枚举值、长度和乱序 IAP 报文，主机不得依赖其行为。
- 除编码器 ACK 等明确规定为幂等的命令外，不应通过重复发送推测命令结果。

### 3.6 启动与主机初始化

设备上电后先显示当前地址和固件版本启动画面。主机接管显示前，推荐按以下顺序初始化：

1. 轮询 `READ_ID`，校验 `77 89`，读取 major/minor 固件版本；
2. 读取 `READ_BUFCOUNT`，等待设备可接收命令；
3. 发送 `SET_SLEEP=0`、`INVOFF`、`SET_BYTESWAP=0`；
4. 设置所需亮度和旋转方向；
5. 显式设置 `CASET/RASET` 和绘图色后开始绘图；
6. 确认连接的是 Unit LCD Plus 后使用 `0xB0..0xB5` 输入命令。

`READ_ID` 当前不返回机型；主机必须由产品配置确认机型，再根据对应固件版本
判断新增命令的支持情况。不得把 major 字节解释为 `MODEL_ID`。

除 I2C 地址外，显示控制和输入配置均应视为运行期状态。主机不应依赖复位前的亮度、
旋转、反色、休眠、字节交换、窗口、绘图色、编码器位置或长按阈值。

## 4. 数据表示

### 4.1 坐标和矩形

- 显示命令中的 X、Y 坐标均为无符号 8 位整数。
- 矩形端点采用闭区间：`XS..XE`、`YS..YE` 均包含端点。
- 矩形宽度为 `XE - XS + 1`，高度为 `YE - YS + 1`。
- 正常方向下有效范围为 X `0..134`、Y `0..239`。
- 旋转 1、3 后逻辑宽高交换；主机应以当前旋转方向对应的宽高限制坐标。
- 为保证兼容性，主机应发送 `XS <= XE`、`YS <= YE`，并确保复制目标完整位于画布内。

`CASET`/`RASET` 定义一个流式读写窗口。RAW/RLE 和像素读回从左到右、从上到下推进，
到达窗口末尾后回绕到窗口起点。

### 4.2 颜色格式

默认 `SET_BYTESWAP=0` 时，颜色字节顺序如下：

| 格式 | 每像素字节 | 线上顺序 | 示例：红色 |
| --- | ---: | --- | --- |
| RGB332 | 1 | `RRRGGGBB` | `E0` |
| RGB565 | 2 | `MSB LSB` | `F8 00` |
| RGB888 | 3 | `R G B` | `FF 00 00` |
| ARGB8888 | 4 | `A R G B` | `80 FF 00 00`（50% 红） |
| A8 | 1 | `A`，RGB 使用最近一次绘图色 | `80` |

`SET_BYTESWAP=1` 时，多字节颜色的线上顺序反转：

- RGB565：`LSB MSB`；
- RGB888：`B G R`；
- ARGB8888：`B G R A`；
- RGB332 和 A8 不变。

`SET_BYTESWAP` 同时影响多字节颜色写入和 `READ_RAW_16/24` 读回，设备复位后恢复 0。

ARGB8888 和 A8 的 Alpha 定义：`0x00` 为完全透明，`0xFF` 为完全不透明。Alpha 绘制
会与画布现有像素混合。

### 4.3 非颜色整数

- 编码器协议中的 16/32 位整数使用大端序。
- IAP 固件总长度和 CRC32 使用大端序。
- 事件位置和旋转增量为有符号整数，其余计数为无符号整数。

## 5. 命令总表

`请求长度` 包含命令字节。`变长` 表示必须由 STOP 结束本次数据流。

| 命令 | 名称 | 请求长度 | 响应 | 功能 |
| --- | --- | ---: | ---: | --- |
| `0x00` | `NOP` | 1..变长 | 0 | 忽略本事务剩余数据 |
| `0x04` | `READ_ID` | 1 | 4 | 读取设备 ID 和固件版本 |
| `0x09` | `READ_BUFCOUNT` | 1 | 1 | 读取命令缓冲区余量 |
| `0x0A` | `READ_MAC` | 1 | 6 | 读取芯片出厂 eFuse MAC，固件 1.2 起支持 |
| `0x20` | `INVOFF` | 1 | 0 | 关闭颜色反转 |
| `0x21` | `INVON` | 1 | 0 | 开启颜色反转 |
| `0x22` | `BRIGHTNESS` | 2 | 0 | 设置背光亮度 |
| `0x23` | `COPYRECT` | 7 | 0 | 复制矩形区域 |
| `0x2A` | `CASET` | 3 | 0 | 设置 X 窗口 |
| `0x2B` | `RASET` | 3 | 0 | 设置 Y 窗口 |
| `0x36` | `ROTATE` | 2 | 0 | 设置旋转/翻转方向 |
| `0x38` | `SET_POWER` | 2 | 0 | 设置功耗模式 |
| `0x39` | `SET_SLEEP` | 2 | 0 | 休眠或唤醒 LCD |
| `0x3A` | `SET_BYTESWAP` | 2 | 0 | 设置颜色字节交换 |
| `0x41..0x45` | `WRITE_RAW_*` | 变长 | 0 | 写入原始像素流 |
| `0x49..0x4D` | `WRITE_RLE_*` | 变长 | 0 | 写入 RLE 像素流 |
| `0x50` | `RAM_FILL` | 1 | 0 | 使用当前颜色填充当前窗口 |
| `0x51..0x54` | `SET_COLOR_*` | 2..5 | 0 | 设置当前绘图色 |
| `0x60..0x64` | `DRAWPIXEL_*` | 3..7 | 0 | 绘制单个像素 |
| `0x68..0x6C` | `FILLRECT_*` | 5..9 | 0 | 填充矩形 |
| `0x81..0x83` | `READ_RAW_*` | 1 | 变长 | 连续读取当前窗口像素 |
| `0xA0` | `CHANGE_ADDR` | 4 | 0 | 修改并持久化 I2C 地址 |
| `0xB0` | `READ_ENCODER_STATE` | 1 | 8 | 读取编码器和队列状态 |
| `0xB1` | `PEEK_ENCODER_EVENT` | 1 | 10 | 查看队首事件 |
| `0xB2` | `ACK_ENCODER_EVENT` | 3 | 0 | 确认并删除队首事件 |
| `0xB3` | `SET_ENCODER_POSITION` | 5 | 0 | 设置位置并清空队列统计 |
| `0xB4` | `READ_BUTTON_CONFIG` | 1 | 2 | 读取长按阈值 |
| `0xB5` | `SET_BUTTON_CONFIG` | 3 | 0 | 设置长按阈值 |
| `0xF0` | `UPDATE_BEGIN` | 8 | 0 | 开始 IAP，声明固件总长度 |
| `0xF1` | `UPDATE_DATA` | 8 + 1..4096 | 1（轮询） | 写入一个固件数据块 |
| `0xF2` | `UPDATE_END` | 4 | 0 | 完成 IAP 并重启 |
| `0xFF` | `RESET` | 4 | 0 | 软件复位 |

## 6. 设备信息与流量控制

### 6.1 `0x04 READ_ID`

请求 `04`，读取 4 字节：

```text
Byte 0      0x77
Byte 1      0x89
Byte 2      firmware_major
Byte 3      debug_flag | firmware_minor
```

Byte 3 的 bit 7 是 debug 标志，bits 6..0 是 minor 版本。当前 release 1.2 返回
`77 89 01 02`，debug 1.2 返回 `77 89 01 82`，IAP 测试固件 1.3 返回 `77 89 01 03`。
Byte 2 是 major 版本，不是 `MODEL_ID`。旧文档中的 MODEL_ID/连续版本 5、6 是历史格式。
已发布的 `Panel_M5UnitLCD` 只校验前两个字节，因此响应仍保持原来的 4 字节长度。

### 6.2 `0x09 READ_BUFCOUNT`

请求 `09`，读取 1 字节。该字节为 `1..255` 的队列余量指标。连续轮询时，每次事务
重新发送 `09`；不要把它解释为剩余 RAM 字节数。

### 6.3 `0x0A READ_MAC`

固件 1.2 起支持。请求只有一个命令字节 `0A`，无参数，主机读取恰好 6 字节。
响应是芯片出厂烧录在 eFuse 中的 MAC-48，按常见 MAC 文本的从左到右顺序返回：

```text
请求        0A
响应示例    AA BB CC DD EE FF
文本表示    AA:BB:CC:DD:EE:FF
```

上述 MAC 仅示意字节顺序。响应不包含命令回显、长度、冒号、字符串结束符或填充字节。
支持 STOP 分开的写后读，也支持重复 START 的组合事务；每次读事务从 MAC 的第一个字节开始。
超过 6 字节时按当前固定响应机制循环输出；主机应只读 6 字节。
该值在上电时读取一次并缓存在内部 DRAM 中，I2C ISR 不调用 eFuse/Wi-Fi 接口；
读取不会启动 Wi-Fi、修改显示所有权或占用绘图命令队列，且不受 I2C 地址修改或重新刷写固件影响。
返回的是出厂 MAC，不是可能由应用另行设置的网络接口 MAC。

旧固件把 `0x0A` 视为未定义命令；仅收到 6 字节不能证明支持该命令。
主机应先确认连接的产品及其 major/minor 固件版本，示例见 `examples/ReadMac/ReadMac.ino`。

## 7. 显示控制

### 7.1 反色

| 命令 | 报文 | 行为 |
| --- | --- | --- |
| `INVOFF` | `20` | 关闭 LCD 面板颜色反转 |
| `INVON` | `21` | 开启 LCD 面板颜色反转 |

反色作用于面板输出，不改变画布中保存的 RGB 像素值。

### 7.2 `0x22 BRIGHTNESS`

```text
22 brightness
```

`brightness` 范围 `0..255`：0 为背光关闭，255 为最大亮度。默认启动亮度为 128。
该设置不写入 NVS，设备复位后恢复默认值。

### 7.3 `0x36 ROTATE`

```text
36 direction
```

| direction | 行为 |
| ---: | --- |
| 0 | 正常方向 |
| 1 | 旋转 90 度 |
| 2 | 旋转 180 度 |
| 3 | 旋转 270 度 |
| 4..7 | 分别为 0..3 的上下翻转形式 |

旋转改变后续绘图和读回使用的逻辑坐标系，不会自动重绘主机内容。

### 7.4 `0x38 SET_POWER`

```text
38 mode
```

| mode | 协议定义 |
| ---: | --- |
| 0 | 低功耗/低速 |
| 1 | 正常 |
| 2 | 高性能 |

兼容性说明：固件版本 5 接收并保持这三个模式的协议兼容，但 ESP32-C61 正式版本因当前
Arduino Core 的安全变频路径限制，CPU 实际固定为 160 MHz。主机不得依赖版本 5 上的
模式切换产生实际频率或功耗差异。

### 7.5 `0x39 SET_SLEEP`

```text
39 state
```

- `state=0`：唤醒 LCD，并恢复休眠前设置的背光亮度；
- `state!=0`：LCD 休眠，同时关闭背光。

休眠不清空画布。建议唤醒后等待面板稳定，再判断显示结果。

### 7.6 `0x3A SET_BYTESWAP`

```text
3A enabled
```

- `enabled=0`：使用第 4.2 节默认颜色顺序；
- `enabled!=0`：反转多字节颜色的线上字节顺序。

## 8. 显示窗口与区域复制

### 8.1 `0x2A CASET`

```text
2A XS XE
```

设置 RAW/RLE 写入和 READ_RAW 读回的 X 闭区间。

### 8.2 `0x2B RASET`

```text
2B YS YE
```

设置 RAW/RLE 写入和 READ_RAW 读回的 Y 闭区间。窗口设置完成后，流指针从
`(XS, YS)` 开始。

### 8.3 `0x23 COPYRECT`

```text
23 SRC_XS SRC_YS SRC_XE SRC_YE DST_X DST_Y
```

复制大小为：

```text
width  = SRC_XE - SRC_XS + 1
height = SRC_YE - SRC_YS + 1
```

源区域左上角复制到 `(DST_X, DST_Y)`。主机必须保证源端点有序且源、目标区域均完整
位于当前逻辑画布内。命令完成后可通过 `READ_RAW_*` 验证目标像素。

## 9. 当前绘图色与基本绘图

### 9.1 设置当前绘图色

| 命令 | 格式 | 报文 |
| --- | --- | --- |
| `0x51 SET_COLOR_8` | RGB332 | `51 C8` |
| `0x52 SET_COLOR_16` | RGB565 | `52 C16[0] C16[1]` |
| `0x53 SET_COLOR_24` | RGB888 | `53 R G B` |
| `0x54 SET_COLOR_32` | ARGB8888 | `54 A R G B` |

指定颜色的 `DRAWPIXEL_*`、`FILLRECT_*` 和 RAW/RLE 像素也会更新当前绘图色。
`WRITE_RAW_A`、`WRITE_RLE_A`、`RAM_FILL`、`DRAWPIXEL` 和 `FILLRECT` 使用当前绘图色。

### 9.2 `0x50 RAM_FILL`

```text
50
```

使用当前绘图色填充 `CASET/RASET` 选择的整个闭区间。

### 9.3 单像素绘制

| 命令 | 格式 | 报文 |
| --- | --- | --- |
| `0x60 DRAWPIXEL` | 当前色 | `60 X Y` |
| `0x61 DRAWPIXEL_8` | RGB332 | `61 X Y C8` |
| `0x62 DRAWPIXEL_16` | RGB565 | `62 X Y C16[0] C16[1]` |
| `0x63 DRAWPIXEL_24` | RGB888 | `63 X Y R G B` |
| `0x64 DRAWPIXEL_32` | ARGB8888 | `64 X Y A R G B` |

### 9.4 矩形填充

| 命令 | 格式 | 报文 |
| --- | --- | --- |
| `0x68 FILLRECT` | 当前色 | `68 XS YS XE YE` |
| `0x69 FILLRECT_8` | RGB332 | `69 XS YS XE YE C8` |
| `0x6A FILLRECT_16` | RGB565 | `6A XS YS XE YE C16[0] C16[1]` |
| `0x6B FILLRECT_24` | RGB888 | `6B XS YS XE YE R G B` |
| `0x6C FILLRECT_32` | ARGB8888 | `6C XS YS XE YE A R G B` |

矩形端点包含在填充范围内。ARGB8888 使用 Alpha 与原画布混合。

示例：用 RGB565 红色填充 X `16..31`、Y `32..47`：

```text
6A 10 20 1F 2F F8 00
```

## 10. RAW 像素流

发送 `CASET` 和 `RASET` 后，用一个独立的连续写事务发送：

| 命令 | 格式 | 每像素字节 |
| --- | --- | ---: |
| `0x41 WRITE_RAW_8` | RGB332 | 1 |
| `0x42 WRITE_RAW_16` | RGB565 | 2 |
| `0x43 WRITE_RAW_24` | RGB888 | 3 |
| `0x44 WRITE_RAW_32` | ARGB8888 | 4 |
| `0x45 WRITE_RAW_A` | A8 + 当前 RGB | 1 |

事务格式：

```text
START + SLA(W) + WRITE_RAW_cmd + pixel_0 + pixel_1 + ... + STOP
```

像素按窗口内从左到右、从上到下写入。超过窗口像素数时会回绕并覆盖窗口起点，主机
应发送与目标窗口相符的像素数。

示例：设置 X `10..11`、Y `20..20`，写入红、蓝两个 RGB888 像素：

```text
2A 0A 0B 2B 14 14
43 FF 00 00 00 00 FF
```

第一行可在同一固定长度写事务中连续发送；第二行是不定长度 RAW 事务，必须用 STOP 结束。

## 11. RLE 像素流

| 命令 | 格式 | 颜色字节数 N |
| --- | --- | ---: |
| `0x49 WRITE_RLE_8` | RGB332 | 1 |
| `0x4A WRITE_RLE_16` | RGB565 | 2 |
| `0x4B WRITE_RLE_24` | RGB888 | 3 |
| `0x4C WRITE_RLE_32` | ARGB8888 | 4 |
| `0x4D WRITE_RLE_A` | A8 + 当前 RGB | 1 |

RLE 流由以下两种记录组成。

重复记录：

```text
count(1..255) + one_color(N bytes)
```

表示连续写入 `count` 个相同颜色像素。

直接记录：

```text
00 + literal_count(1..255) + color_0 + ... + color_(literal_count-1)
```

表示后续包含 `literal_count` 个不压缩颜色。直接记录结束后自动回到重复记录模式。
所有记录必须完整包含在同一 RLE 写事务内，并由 STOP 结束。

RGB565 示例：7 个红色，随后直接写绿/蓝/红，再写 4 个蓝色：

```text
4A
07 F8 00
00 03 07 E0 00 1F F8 00
04 00 1F
```

RLE 解码后的像素推进、窗口回绕和 Alpha 行为与 RAW 相同。

## 12. 像素读回

先设置 `CASET/RASET`，再发送读命令并读取所需的完整像素数：

| 命令 | 格式 | 每像素响应字节 |
| --- | --- | ---: |
| `0x81 READ_RAW_8` | RGB332 | 1 |
| `0x82 READ_RAW_16` | RGB565 | 2 |
| `0x83 READ_RAW_24` | RGB888 | 3 |

读指针从窗口左上角开始，从左到右、从上到下推进，并在窗口末尾回绕。主机用读事务
长度决定读取多少像素。

读取 `(2,2)` 的一个 RGB888 像素：

```text
Write: 2A 02 02 2B 02 02
Write/read: 83 -> read 3 bytes
```

若像素为红色且 `SET_BYTESWAP=0`，响应为 `FF 00 00`。

注意：画布读回不受面板 `INVON` 的视觉反色影响。读回前必须等待先前绘图命令被处理。

## 13. 地址修改与软件复位

### 13.1 `0xA0 CHANGE_ADDR`

```text
A0 NEW_ADDR (~NEW_ADDR & FF) A0
```

生效条件：

- `NEW_ADDR` 在 `0x08..0x77`；
- 校验字节等于新地址的 8 位按位取反；
- 首尾命令字节均为 `0xA0`；
- 新地址与当前地址不同。

合法命令会把地址写入 NVS 并自动重启，重启后只响应新地址。非法命令无副作用。

示例：`0x3E -> 0x3F`：

```text
A0 3F C0 A0
```

地址掉电保存，软件复位和 IAP 后仍保留。测试程序如临时修改地址，结束前必须恢复
`0x3E`。

### 13.2 `0xFF RESET`

```text
FF 77 89 FF
```

校验正确时立即软件复位。建议主机等待至少 300 ms，并重复轮询 `READ_ID`，直到设备
重新响应。复位不清除持久化 I2C 地址，但会清除运行期显示控制、编码器队列、位置和
长按配置。

## 14. 编码器与按键

### 14.1 输入时序和事件

| 项目 | 定义 |
| --- | --- |
| 采样周期 | 1 ms |
| 按键消抖 | 10 ms |
| 双击窗口 | 固定 300 ms |
| 长按默认值 | 500 ms |
| 长按可设置范围 | 500..5000 ms |
| 事件队列 | 固定 64 条 |
| 旋转增量 | 每个正交边沿 `+1` 或 `-1`（每个机械卡位 2 个计数），无加速 |
| 默认旋转方向 | 顺时针旋转时 `position` 增加，逆时针旋转时减少 |

旋钮从一个稳定卡位移动到相邻卡位时，编码器产生两个正交边沿；固件对每个边沿产生一个
`+1` 或 `-1`，因此一个机械卡位恰好对应 2 个计数。旋钮静止在卡位时 `position` 为偶数，
奇数值只在两个卡位之间短暂出现。固件按最高分辨率上报：只需要卡位计数的主机可以将
`position` 除以 2，或将成对的 `ROTATE` 事件合并；反之主机无法从较低的分辨率恢复边沿信息。
计数倍率与编码器每旋转一周的脉冲数是两个独立概念，后者不改变单个卡位的计数倍率。

事件类型：

| 值 | 名称 | 说明 |
| ---: | --- | --- |
| 0 | `NONE` | 队列为空 |
| 1 | `ROTATE` | 编码器正交边沿（每个机械卡位 2 次） |
| 2 | `PRESS` | 消抖后按下 |
| 3 | `RELEASE` | 消抖后释放 |
| 4 | `SINGLE_CLICK` | 双击窗口结束后确认单击 |
| 5 | `LONG_PRESS` | 达到长按阈值 |
| 6 | `DOUBLE_CLICK` | 双击完成 |

事件顺序：

```text
单击：PRESS -> RELEASE -> [等待 300 ms] -> SINGLE_CLICK
双击：PRESS -> RELEASE -> PRESS -> RELEASE -> DOUBLE_CLICK
长按：PRESS -> LONG_PRESS -> RELEASE
```

长按不产生单击或双击；旋转和按键事件按实际时间顺序进入同一队列。

### 14.2 `0xB0 READ_ENCODER_STATE`

请求 `B0`，响应 8 字节：

```text
Byte 0      flags
Byte 1      pending_count, 0..64
Byte 2..5   position, int32 BE
Byte 6..7   dropped_count, uint16 BE
```

`flags`：

| 位 | 名称 | 含义 |
| ---: | --- | --- |
| bit0 | `BUTTON_PRESSED` | 当前按键已按下 |
| bit1 | `BUTTON_LONG_PRESSED` | 当前按下已触发长按 |
| bit2 | `EVENT_PENDING` | 队列非空 |
| bit3..6 | Reserved | 固定为 0 |
| bit7 | `QUEUE_OVERFLOW` | 自上次清除后发生过事件丢弃 |

响应是同一时刻的原子快照。

### 14.3 `0xB1 PEEK_ENCODER_EVENT`

请求 `B1`，响应 10 字节：

```text
Byte 0      event_type
Byte 1      event_flags
Byte 2..3   sequence, uint16 BE
Byte 4..5   delta, int16 BE
Byte 6..9   position, int32 BE
```

非旋转事件 `delta=0`。`position` 是事件发生时的位置。空队列返回 `type=NONE`、
`sequence=0xFFFF`、`delta=0` 和当前位置。PEEK 不删除事件，重复 PEEK 返回同一队首。

### 14.4 `0xB2 ACK_ENCODER_EVENT`

```text
B2 SEQUENCE_MSB SEQUENCE_LSB
```

仅当序号等于当前队首时删除该事件。错误、重复、过期或尚未到达队首的 ACK 均无副作用。
主机只有在完整处理事件后才可 ACK；通信失败时不得猜测 ACK。

推荐循环：

```text
PEEK -> 处理事件 -> ACK 同一 sequence -> 再次 PEEK
```

### 14.5 `0xB3 SET_ENCODER_POSITION`

```text
B3 POSITION[31:24] POSITION[23:16] POSITION[15:8] POSITION[7:0]
```

位置为 int32 BE。位置到达 int32 边界后饱和。该命令原子设置位置，并清空事件队列、
`dropped_count`、溢出标志和待确认单击；不改变当前物理按键状态、下一事件序号或长按配置。

队列满时丢弃最旧事件、保留最新事件，`dropped_count` 饱和于 65535。事件序号为
uint16，从 0 开始并允许 `65535 -> 0` 回绕。

### 14.6 `0xB4 READ_BUTTON_CONFIG`

请求 `B4`，响应 2 字节 `long_press_ms`（uint16 BE）。

### 14.7 `0xB5 SET_BUTTON_CONFIG`

```text
B5 LONG_PRESS_MSB LONG_PRESS_LSB
```

- 单位 ms，有效范围 `500..5000`；
- 无效值不改变现有配置；
- 设置后应使用 `B4` 回读；
- 若设置时按键已按下，本次动作仍使用 PRESS 时锁存的旧阈值；
- 配置不写 NVS，复位后恢复 500 ms；
- 不清空事件队列或溢出统计。

编码器和按键的完整边界语义及更多报文示例见
更多编码器与按键报文示例见 `docs/encoder_button_protocol_zh.md`。

## 15. I2C 在线固件升级（IAP）

IAP 会写入下一个 OTA 分区。升级前必须确认固件镜像适用于 Unit LCD Plus ESP32-C61，
且镜像大小不超过目标 OTA 分区。升级期间不得发送显示或编码器命令。

### 15.1 CRC32 定义

每个数据块独立计算 CRC32：

| 项目 | 值 |
| --- | --- |
| 多项式 | `0x04C11DB7` |
| 初始值 | `0xFFFFFFFF` |
| 处理方向 | MSB-first |
| 输入反射 | 无 |
| 输出反射 | 无 |
| 最终异或 | 无 |

伪代码：

```text
crc = 0xFFFFFFFF
for byte in block:
    crc = (crc << 8) ^ table[((crc >> 24) ^ byte) & 0xFF]
```

### 15.2 `0xF0 UPDATE_BEGIN`

当前固件版本 5 实际请求长度为 8 字节：

```text
F0 77 89 F0 SIZE[31:24] SIZE[23:16] SIZE[15:8] SIZE[7:0]
```

`SIZE` 是完整固件二进制长度，uint32 BE。设备初始化 OTA、清零写入偏移并进入等待数据
状态。旧 M5GFX 头文件中把本命令注释为 4 字节，该注释不适用于当前固件。

### 15.3 `0xF1 UPDATE_DATA`

每块最大 4096 字节，最后一块为剩余长度。一个数据块必须在同一写事务内发送：

```text
F1 77 89 F1 CRC[31:24] CRC[23:16] CRC[15:8] CRC[7:0]
DATA[0] ... DATA[N-1]
STOP
```

其中 `N = min(4096, SIZE - current_offset)`。整个报文长度为 `8 + N`，CRC 只覆盖 N 字节
DATA，不覆盖 8 字节头。

发送完一块后，主机以纯读事务轮询 1 字节结果，不重新发送 F1：

```text
START + SLA(R) + result + STOP
```

| result | 名称 | 含义 |
| ---: | --- | --- |
| `0xFF` | `BUSY` | CRC 正确，正在写 Flash，继续轮询 |
| `0xF1` | `OK` | 本块写入成功，可发送下一块 |
| `0x01` | `BROKEN` | 本块 CRC 不匹配，终止本轮升级 |
| `0x00` | `ERROR` | 状态、参数或 Flash 写入错误，终止本轮升级 |

必须等到 `OK` 后才发送下一块。官方测试每 10 ms 轮询，并为每块设置 3 秒超时。

### 15.4 `0xF2 UPDATE_END`

主机必须确认所有 `SIZE` 字节均成功写入后才能发送：

```text
F2 77 89 F2
```

设备依赖主机遵守上述长度约束，不应把 `UPDATE_END` 当作固件长度校验命令。随后设备
写回先前保留的镜像头、设置新 OTA 启动分区，并约 1 秒后自动重启。主机应等待设备
重新上线，再通过 `READ_ID` 确认目标版本。若 UPDATE_END 失败，设备不会把不完整分区设为
启动分区。

### 15.5 IAP 主机流程

1. `READ_ID` 确认设备和当前版本。
2. 读取固件文件并确认总长度、目标芯片和发布版本。
3. 发送 `UPDATE_BEGIN`。
4. 从 offset 0 开始，按最大 4096 字节分块。
5. 对当前块计算本节 CRC32。
6. 单事务发送 `UPDATE_DATA` 头和数据。
7. 轮询结果；仅在 `OK` 后增加 offset。
8. 全部数据完成后发送 `UPDATE_END`。
9. 等待重启并用 `READ_ID` 验证版本。

IAP 不改变持久化 I2C 地址。通信中断、CRC 错误或超时后，应终止本轮流程，并从
`UPDATE_BEGIN` 重新开始完整升级，不能从主机猜测的偏移继续。

## 16. 主机实现检查清单

- 使用 7 位地址 `0x3E` 和不高于 400 kHz 的 I2C。
- 固定长度命令不可被 STOP/重复 START 截断。
- RAW、RLE 和 IAP 数据必须作为单个连续写事务发送。
- 发送大量显示命令时轮询 `READ_BUFCOUNT`。
- 坐标按闭区间计算，区域复制端点必须有序且不越界。
- 明确设置并记录 `SET_BYTESWAP`，避免 RGB565/RGB888 通道错误。
- 像素读回前等待异步绘图完成。
- 编码器位置和 delta 按有符号大端整数解析。
- 编码器事件必须 PEEK、处理、再按队首序号 ACK。
- 接受事件序号回绕，监控 overflow 和 dropped_count。
- 设置长按阈值后使用 `READ_BUTTON_CONFIG` 回读。
- 修改地址后等待重启，并在新地址重新执行 `READ_ID`。
- IAP 每块独立计算 CRC，必须等待 `OK`，升级后必须校验版本。

## 17. 版本兼容性

- 本分支 ESP32 和 ESP32-C61 构建使用 major/minor 版本：release 为 1.2，IAP 测试固件为 1.3。
- 显示、读回、地址、复位和 IAP 命令编号保持不变，`READ_ID` 仍为 4 字节。
- `READ_ID` Byte 2 为 major；Byte 3 的 bit 7 为 debug 标志，bits 6..0 为 minor。
  历史文档中的 MODEL_ID 和连续版本号 5、6 不适用于当前固件。
- `0xB0..0xB5` 是 Unit LCD Plus 的编码器/按键命令；机型须由主机产品配置确认。
- `0x0A READ_MAC` 从 1.2 起支持；旧主机不发送该指令时行为不变。
- `SET_POWER` 保持协议兼容，但 C61 实际固定 160 MHz。
- 编码器长按配置只在本次运行有效；I2C 地址和本机 UI 主题可持久化，MAC 由出厂 eFuse 确定。
- 主机不得利用未定义命令的响应推断能力。

## 18. 实现依据与变更记录

### 18.1 实现依据

本文与以下仓库内容交叉核对：

- `src/command_processor.cpp`：命令长度、解析、显示、读回、地址、复位和 IAP 状态机；
- `src/update.cpp`、`src/update.hpp`：4096 字节块、CRC32 和 OTA 写入；
- `src/cpu_clock.cpp`：ESP32-C61 功耗模式限制；
- `include/unit_lcd_plus_protocol.hpp`：`0xB0..0xB5`、响应长度、标志和事件值；
- M5GFX 0.2.26 `Panel_M5UnitLCD`：原 Unit LCD 命令编号和主机事务；
- `examples/BasicProtocolTest`：显示、读回、地址、复位和 IAP 的已验证主机流程；
- `examples/BasicFullFunctionTest`：完整显示及编码器/按键交互示例；
- `docs/evidence/basic_protocol_0_5_20260812.log` 和
  `docs/evidence/basic_protocol_iap_0_5_0_6_20260812.log`：硬件回归记录。

### 18.2 变更记录

| 文档版本 | 日期 | 变更 |
| --- | --- | --- |
| 1.0 | 2026-08-12 | 首版统一规范；整合原显示协议、Unit LCD Plus 输入协议和当前 8 字节 IAP 头 |
| 1.1 | 2026-08-20 | 将 `READ_ID` Byte 2 定义为 `MODEL_ID`，Byte 3 定义为连续固件版本号 |
| 1.2 | 2026-09-08 | 编码器按键改为 GPIO9；上电按住可进入 Joint Download Boot |
| 1.3 | 2026-11-17 | 新增 `0x0A READ_MAC`；固件升至 1.2；同步当前 READ_ID major/minor 定义 |
