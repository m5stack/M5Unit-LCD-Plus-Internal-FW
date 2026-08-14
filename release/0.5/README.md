# Unit LCD Plus 0.5 固件交付

正式固件为完整合并镜像，烧录起始地址为 `0x0000`。

| 文件 | 烧录目标 | 镜像布局 |
| --- | --- | --- |
| `unit_lcd_plus_release_0.5_0x00.bin` | Unit LCD Plus ESP32-C61 | Bootloader `0x0000`、分区表 `0x8000`、boot_app0 `0xE000`、应用 `0x10000` |

## SHA256

```text
a5bcdddf7ea666cdfb53f56c971ef7a41660919417d6c0d72bf576a261916657  unit_lcd_plus_release_0.5_0x00.bin
```
