# Unit LCD Plus 内置 UI 预览

这是 Unit LCD Plus（ESP32-C61HF4、旋钮 + 按键、135 x 240 竖屏）的 LVGL 视觉候选和实现契约。
本目录的原型用于先确定视觉方向；正式固件接入在视觉方向确认后进行。

## 目标画像

- 产品：Unit LCD Plus，仅考虑 ESP32-C61 形态；旧 ESP32 Unit LCD 不纳入本 UI。
- 显示：ST7789 类 135 x 240 面板，现有 M5GFX 保留为显示传输层。
- 输入：编码器 A/B = GPIO2/GPIO3；按键 GPIO9，低有效。每个机械卡位上报两个边沿事件。
- 通信：设备继续作为 I2C 从机，默认地址 `0x3E`，合法地址范围 `0x08..0x77`。
- 首要任务：上电后让用户确认型号、固件版本、当前地址和主机等待状态；无需主机即可修改地址。
- 字体：原型使用 LVGL 内置 Montserrat 10/12/14/18/24/28；正式版如果需要中文，必须另行提供字体覆盖和内存测量。

## 视觉候选

三套候选共用同一信息架构、屏幕尺寸和旋钮交互，只改变 token 与层级表达。选择 gate 完成前不把某一套视为正式品牌方向。

| 名称 | 一句话概念 | 主色 | 适合场景 |
| --- | --- | --- | --- |
| `precision` | 深色精密仪表：青色焦点轨和低噪声信息层级 | `#20D6C7` | 工程调试、长期连接主机 |
| `utility` | 浅色现场工具：橙色操作色、青绿色状态色（**出货默认**） | `#F15D36` | 明亮环境、快速识别按钮 |
| `console` | 深色现场控制台：黄绿色焦点、编号菜单行 | `#B7ED55` | 诊断和硬件实验室 |

方向 gate 已关闭：`utility` 为默认主题。三套主题保留为运行时可切换项——
Home 第三行 `THEME` 进入选择页，点选即整屏实时换肤（选择页本身就是预览），
长按返回；固件通过 command_processor 的 NVS 路径（`UI_THEME` 键）持久化，
掉电重启后保持，UI 侧不直接触碰 NVS。

每套候选都包含：

1. Home：`LOCAL`、`WAITING FOR HOST`/`READY / WAIT`、版本和地址，以及
   `ADDRESS` / `DEVICE` / `THEME` 三个选项行（页脚行已并入 DEVICE 行）。
2. Address Edit：旋钮在 `0x08..0x77` 内逐步调整地址。
3. Confirm：旧地址、新地址和重启警告。
4. Saving / Saved / Not Saved：保存反馈和失败重试。
5. Device Info：Unit LCD Plus、ESP32-C61、固件版本、135 x 240、Back。
6. Theme：PRECISION / UTILITY / CONSOLE 三行选择页，当前主题带勾。

Home 上按旋钮选择 `ADDRESS`、`DEVICE` 或 `THEME`；按下后进入；旋钮编辑地址或切换主题；
`Esc`/返回动作回到上一级。
正式固件的输入适配必须读取独立的本地 observer/快照，不能对主机事件队列执行 `PEEK -> ACK`。

## Token

所有坐标以 135 x 240 原生像素计算。内容区在 `x=6..129`（候选按主题使用 119/121/123 px 宽度），
行高 34 px，顶栏高 30 px，重点地址数字使用 28 px 字体。按钮和信息行保持稳定尺寸，避免数字变化造成布局跳动。

- 背景：深色候选约 `#101416`/`#0D100C`；浅色候选 `#F2F4F3`。
- 表面：单层 panel + row，不嵌套卡片；边框 1 px；圆角 0、3 或 6 px。
- 文本：主文本高对比，辅助文本用于状态和元信息；字距固定为 0。
- 状态：warning 橙色、danger 红色、success 绿色；颜色之外同时使用符号、边框或焦点轨。

## 状态与所有权

```text
BOOT_UI / LOCAL
      |
      | 第一个完整且合法、已成功入队的主机显示写事务
      v
HOST_PENDING -> HOST_OWNED
      |
      +-- RESET / esp_restart --> BOOT_UI
      +-- IAP(F0/F1/F2) --------> IAP（冻结本地 UI 与普通 flush）
```

接管判断必须发生在完整命令解析并成功入队之后；I2C ISR 只置位原子通知，LVGL 对象只能由 loop 所属任务操作。
不能以首字节或任意写事务触发接管。

不会触发显示接管的只读/探测命令：`READ_ID (04)`、`READ_BUFCOUNT (09)`、`READ_RAW (81..83)`、
`READ_ENCODER_STATE (B0)`、`PEEK_ENCODER_EVENT (B1)`、`READ_BUTTON_CONFIG (B4)`、`NOP (00)`、
未知命令、短包和校验失败报文。

建议触发 `HOST_PENDING` 的完整有效命令：

- 控制/会话：`INVOFF/INVON (20/21)`、`BRIGHTNESS (22)`、`ROTATE (36)`、`SET_POWER (38)`、
  `SET_SLEEP (39)`、`SET_BYTESWAP (3A)`；
- 窗口/颜色准备：`CASET (2A)`、`RASET (2B)`、`SET_COLOR (51..54)`；
- 实际绘图：`RAW (41..45)`、`RLE (49..4D)`、`RAM_FILL (50)`、`DRAWPIXEL (60..64)`、
  `FILLRECT (68..6C)`、`COPYRECT (23)`；
- 若产品决定让主机独占输入，再单独把 `ACK_EVENT (B2)`、`SET_POSITION (B3)`、
  `SET_BUTTON_CONFIG (B5)` 纳入输入所有权；本地 UI 不发送这些命令。

`CHANGE_ADDR (A0)` 必须复用现有校验、NVS commit、停 I2C、重启路径；不得在 UI 中重复实现 NVS。
保存失败保持旧地址。合法 `RESET (FF)` 和 IAP 优先级高于普通显示接管。

## 动效表

动画原语使用 smooth_ui_toolkit（见下节采用记录）；选择器和地址数字都是可重定向
spring，旋钮快速连动时从当前渲染值续动，而不是重启固定时长补间。

| 触发 | 属性 | 时长/参数 | 中断行为 | 代价 |
| --- | --- | --- | --- | --- |
| 旋钮移动（选项切换） | 选择色块 y（整行高 34 px、内容宽度的方形色块，surface_raised 填充 + accent 描边） | spring：visualDuration 0.24 s、bounce 0.2 | 下一次移动从当前值带速度重定向 | 单对象移动，局部重绘 |
| 地址增减 | 两个十六进制滚位列 y（`0x` 前缀静止） | spring：visualDuration 0.3 s、bounce 0.12 | 逐位重定向；滚动方向随增/减语义 | 每列 1 个多行 label，单对象移动 |
| 页面切换 | content y：5 -> 0 | 160 ms，ease-out（原生 lv_anim） | 新页面建立后可再次切换 | 不使用离屏 layer |
| 按键按下 | 色块填充变 accent、文字变 focus_text | 即时 | release 恢复焦点态 | 不做 transform 缩放 |
| Saving | 静态刷新符号 + 状态文案 | 由保存事务驱动 | 成功/失败替换状态页 | 不做旋转动画，避免 layer 分配 |

未聚焦选项保持纯文本（无填充/边框），让移动色块成为唯一焦点指示，符合
"次要信息更安静"的层级规则。

## smooth_ui_toolkit 采用记录

- 版本：`v2.14.0`（commit `3cfe6a373edee2e325b8490491f921ecc71b29e6`，MIT）。
- 集成：`lib/smooth_ui_toolkit/` vendored `src/core` 子集（AnimateValue、spring/easing
  发生器、HAL）。不走 `lib_deps` git：上游 `library.json` 的 `srcDir: "."` 会把
  raylib 示例的 `main()` 编进固件。许可证见 `lib/smooth_ui_toolkit/LICENSE`。
- LVGL：固件 9.5.0 / 模拟器 9.3.0（core 子集不依赖 LVGL 版本）。
- Tick：设备端 `local_ui::init` 中 `ui_hal::on_get_tick` 绑定 `millis()`；模拟器用
  默认 chrono HAL。
- 使用配方：`AnimateValue` spring —— 选择器色块位移、地址十六进制滚位。
- 项目自有组件：`ui/hex_digit_flow.hpp`。上游 `DigitFlow` 只支持十进制字符集，
  地址需要 `0-F`，故用相同的环绕填充列表技术复刻十六进制滚位列；每列是裁剪容器
  内的单个多行 label，每帧只移动一个对象。LVGL 堆分配失败时编辑页回退为静态
  `0xNN` 文本，地址仍可编辑。
- 动效泵：controller 内 16 ms `lv_timer`，与 LVGL 同线程；动画对象不持有 LVGL
  指针，屏幕销毁先停 timer 再删对象。
- 实测：模拟器 smoke 三变体 LVGL 堆峰值约 18 KB（预算 32 KB）；固件 `release`
  RAM 19.8%、Flash 52.2%。


## 资源预算

目标 C61 为单核 160 MHz、320 KB SRAM、4 MB Flash、无可依赖的 PSRAM。
现有主机 RGB888 画布约 `135*240*3 = 97,200` bytes，并且必须留在内部可 DMA/ISR 读取内存。
根据现有 linker map，内部 heap 起点到 SRAM 上限约 163 KB；扣画布和系统占用后余量有限。

- 正式 LVGL 使用 partial draw buffer，不使用 full-screen 双缓冲。
- 初始建议：两条 `135 x 24` RGB565 buffer，共约 12,960 bytes，均为 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`。
- LVGL 对象/字体/动画堆目标 24--32 KB；运行时必须记录 internal/DMA free heap 与 minimum free heap。
- 原生模拟器将 `LV_MEM_SIZE` 限为 32 KB；三套候选普通页峰值约 16 KB，smoke 流程峰值约 18.7 KB。
- M5GFX async DMA 未完成前不得改写其源 canvas，也不得向 LVGL 报告 flush ready；正式 port 必须等待 SPI busy 清除。

## 预览构建与操作

固定使用 LVGL 9.3.0 commit `c033a98afddd65aaafeebea625382a94020fe4a7` 的 SDL 模拟器。
当前阶段不把 LVGL 接入正式 PlatformIO 固件；正式接入时应锁定 LVGL 9.5.0（或与模拟器统一版本），
并在 C61 target build 中重新确认 API、配置和内存占用。

```bash
cmake -S simulator -B .build/ui-preview -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_LVGL=/tmp/lvgl-v9.3.0
cmake --build .build/ui-preview --target unit-lcd-ui-sim -j 4

SDL_VIDEODRIVER=dummy \
  .build/ui-preview/unit-lcd-ui-sim \
  --variant precision --state home --capture .build/ui-preview/captures/precision-home.bmp
```

交互运行时：上/左 = 上一项，下/右/滚轮 = 下一项，Enter/Space/鼠标左键 = 按下/释放，
Esc/Backspace = 返回，`q` = 退出。`--smoke` 自动覆盖设备信息、地址递增、确认、保存成功/失败。
`--stats` 输出 LVGL 堆统计；截图模式会递归检查对象边界和文本宽度。

## 当前验证

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 三变体编译 | PASS | CMake/Ninja `unit-lcd-ui-sim` |
| 3 × 9 状态截图 | PASS | 135 x 240 BMP；home/pressed/edit/confirm/saving/saved/error/about/theme |
| 文本/对象布局检查 | PASS | 模拟器递归 validator，27/27 通过（滚位列标记 `LV_OBJ_FLAG_USER_1`，其换行 label 有意溢出裁剪框，validator 跳过该子树） |
| 交互 smoke 成功保存 | PASS | 三变体 `--smoke`：设备信息、主题实时切换回调、地址递增、确认、保存（堆峰值约 18 KB / 32 KB） |
| 交互 smoke 保存失败 | PASS | 三变体 `--smoke --fail-save` |
| 选择色块/滚位运动帧 | PASS | 模拟器截图确认色块滑行中间帧、滚位列中间帧（新数字从下方滚入） |
| C61 PlatformIO 固件 | PASS | `pio run -e release`：RAM 19.8%（64,836 B）、Flash 52.2%（684,234 B） |
| 旧 ESP32 固件回归 | PASS | `pio run -e esp32_release`：Flash 30.9%（404,907 B） |
| 本地 UI 偏色修复 | PARTIAL | `present_local_ui_tile` 改用 `lgfx::rgb565_t` 重载（原 `uint16_t*` 走 M5GFX `swap565_t` 解释，逐像素字节交换导致品红偏色）；构建通过，待实机复核 |
| 主题 NVS 持久化 | PARTIAL | `UI_THEME` 键读写复用地址 NVS 路径；掉电保持行为待实机复核 |
| 实机屏幕、DMA、功耗、输入 | NOT RUN | 本轮改动后无连接硬件证据 |

## 选择 gate 与后续接入

方向已确定（`utility` 默认，运行时三主题可切换），正式固件接入已完成：
单一 LVGL owner task、partial RGB565 display port、RGB888 host canvas 与
`READ_RAW`/I2C parser 保留、本地编码器输入 sink、`BOOT_UI/HOST_OWNED/IAP`
状态与完整命令入队接管点、地址修改复用 NVS/重启路径。

剩余工作：

1. 补充 RESET、IAP、DMA busy 和队列 backpressure 回归测试。
2. 在 C61 实机上复核偏色修复、主题 NVS 持久化、internal/DMA heap、flush 时序、
   旋钮事件丢失和屏幕撕裂；未测量前不宣称硬件通过。
