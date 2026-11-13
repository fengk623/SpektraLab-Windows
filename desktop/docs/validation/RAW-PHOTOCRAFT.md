# PhotoCraft RAW 真机验收：步骤与记录

对应需求《SpektraLab Windows RAW 解码与线性 TIFF 接入》2026-11-12 v1.0 的 TEST-01 ~ TEST-04、
DEL-02。设计与契约见 `engine/WINDOWS_PHOTOCRAFT_RAW.md`。

本文件有两部分：**怎么测**（可重复的命令），和**测到了什么**（记录表）。记录表目前全部为
“待测”：开发环境里没有相机样片，也没有 Windows 真机 GPU。自动化测试只覆盖合成 DNG。

一条规则：只填实际跑过的结果。没跑的写“待测”，跑不了的写原因，不要按预期填。

## 0. 被测构建

| 项 | 值 |
|---|---|
| 分支 / 提交 SHA | |
| CI 运行链接 | |
| 测试包来源 | 个人仓库 Actions Artifacts `spektralab-windows-x64`（只在 Artifacts，不发 Release） |
| 安装包文件名 / SHA-256 | |
| 便携包文件名 / SHA-256 | |
| `spektralab-host.exe --version` | |
| 报告里的 `photocraft_commit` | 应为 `e052eabf4be74612725400955d184f85d208c0f0` |
| 机器 / CPU / 内存 | |
| GPU / 驱动版本 | |
| Windows 版本（`winver`） | |

提交 SHA 和两个文件的 SHA-256 在该次 CI 运行的摘要页（“Windows test package”）和
“Test package checksums” 步骤的日志里；下载后用 `Get-FileHash -Algorithm SHA256 <文件>`
核对一致再测。

## 1. 样片登记（TEST-01）

样片不入库。登记文件名、SHA-256、来源和授权；可公开的给链接。压缩模式和位深以文件为准：
`libraw_decoder` 取自报告的 `decode.libraw_decoder`，位深和 CFA 取自 `decode.photocraft`。

| # | 机型 | 文件 | SHA-256 | 像素 | 压缩模式（libraw_decoder） | 位深 / CFA | 来源与授权 | 状态 |
|---|---|---|---|---|---|---|---|---|
| 1 | Sony α7R V | | | | | | | 待测 |
| 2 | 富士约 102 MP RAF | | | | | | | 待测 |
| 3 | Canon（CR2；本版 PhotoCraft 不解 CR3） | | | | | | | 待测 |
| 4 | Nikon 无损 NEF | | | | | | | 待测 |
| 5 | DNG | | | | | | | 待测 |
| 6 | 普通照片（JPEG / TIFF，回归用） | | | | — | — | | 待测 |

尼康 HE/HE\*、飞思 IIQ、Sony ARQ：PhotoCraft 本版不解码。手上有样片时各跑一次第 2 节的
PhotoCraft 命令，登记返回的退出码和错误原文即可。

## 2. RAW 转线性 TIFF（RAW-01、TIFF-01~04、DEL-02）

`spektralab-host.exe` 在安装目录根下（CI 的静默安装是 `C:\Program Files\SpektraLab\`），便携包
在解压目录根下。每个样片跑两次，输出用新文件名（已存在的输出不会被覆盖，除非加
`--overwrite`）：

```powershell
$hostExe = 'C:\Program Files\SpektraLab\spektralab-host.exe'   # 便携包：<解压目录>\spektralab-host.exe
$in = 'D:\samples\DSC00001.ARW'; $out = 'D:\raw-out'
New-Item -ItemType Directory -Force $out | Out-Null
& $hostExe raw-to-tiff --input $in --output "$out\DSC00001-photocraft.tif" `
    --report "$out\DSC00001-photocraft.json" --backend photocraft
"exit $LASTEXITCODE"
& $hostExe raw-to-tiff --input $in --output "$out\DSC00001-libraw.tif" `
    --report "$out\DSC00001-libraw.json" --backend libraw
"exit $LASTEXITCODE"
```

退出码：0 成功；3 该解码器不支持此文件；4 解码失败；5 输出已存在或写入失败；6 TIFF 重读与
写入不一致。非 0 时把 stderr 原文记下来。

每个样片从两份报告里抄这些字段：

| 字段（报告路径） | PhotoCraft | LibRaw |
|---|---|---|
| 退出码 | | |
| `decode.width` × `decode.height` | | |
| `decode.libraw_decoder` | | |
| `decode.photocraft.format` / `cfa` | | — |
| `decode.photocraft.white_balance_source` | | — |
| `decode.photocraft.white_balance_multipliers` | | — |
| `decode.photocraft.calibration` | | — |
| `decode.photocraft.black_level` / `white_level` | | — |
| `decode.photocraft.orientation` | | — |
| `decode.photocraft.warnings`（原文） | | — |
| `metadata.missing` | | |
| `pixels.min` / `max` / `mean` | | |
| `pixels.samples_below_zero` / `samples_above_one` | | |
| `tiff.icc_profile_id` | 应为 `266b19b34ea10d114519d955b34e0142` | 同左 |
| `verify.passed` / `verify.max_abs_error` | 应为 true / 0 | 应为 true / 0 |
| `decode.photocraft.timings_ms`（read / decode / develop / copy） | | — |
| `decode.decoder_timings_ms` | | |
| `tiff.write_ms` | | |
| `memory.peak_after_decode_bytes` / `peak_after_tiff_write_bytes` / `peak_after_verify_bytes` | | |

把两个 TIFF 都在应用里打开（或任一支持 32 位浮点 TIFF 和 ICC 的软件），对看并记录：色偏、
方向、裁边（尺寸差）、高光。

## 3. 应用内对比（TEST-02、RAW-02）

应用默认仍走 LibRaw（已验收名单为空）。让整个应用走 PhotoCraft：

```powershell
$env:SPEKTRALAB_RAW_BACKEND = 'photocraft'
& 'C:\Program Files\SpektraLab\spektralab.exe'
```

不设该变量（或设为 `libraw`）重开应用，即为 LibRaw 对照。切换后端必须重开应用：变量在
应用启动时读取一次。磁盘上的显示缓存按后端分开存放，切换后不会显示另一后端留下的画面，
无需手动清缓存。两种后端下，同一样片各做一遍：

| 步骤 | PhotoCraft | LibRaw | 差异 / 备注 |
|---|---|---|---|
| 文件 → 打开 | | | |
| 拖入窗口 | | | |
| 文件夹内切换上一张 / 下一张 | | | |
| As Shot 白平衡下的画面 | | | |
| 手动白平衡（色温 / 色调） | | | |
| 曝光调节 | | | |
| 原图对照 | | | |
| 套用胶片效果 | | | |
| 全分辨率导出（TIFF），导出文件尺寸 | | | |
| 批量导出（含本样片） | | | |
| 镜头校正（如该镜头有配置文件） | | | |
| 中性点取样 | | | |

四种进入方式（打开、拖入、切图、批量）在同一后端下结果应一致。不一致就是缺陷，记下
是哪一种。

## 4. 性能（TEST-03）

RTX 4090 真机，普通照片和约 102 MP 富士样片各测。每项至少 3 次，写出每次的数，不只写
平均。解码时间取第 2 节报告（`timings_ms`），渲染时间取应用内的计时；两者分开记。GPU
利用率和专用 VRAM 用任务管理器或 `nvidia-smi` 单独记。

| 项 | 样片 | 后端 | 第 1 次 | 第 2 次 | 第 3 次 | 备注 |
|---|---|---|---|---|---|---|
| 打开到首帧 | | | | | | |
| 松开曝光滑块到最终预览 | | | | | | |
| 全尺寸导出 | | | | | | |
| 进程峰值 RAM | | | | | | |
| 专用 VRAM 峰值 | | | | | | |
| GPU 利用率（单独记录） | | | | | | |
| 解码：read / decode / develop / copy（ms） | | | | | | 取自报告 |

测时机器上还在跑什么（`Get-Process | Sort CPU -Desc | Select -First 5`）一并记下。

## 5. 失败与恢复（TEST-04）

| 情形 | 做法 | 结果（错误原文） | 之后普通照片能否调参和导出 |
|---|---|---|---|
| 坏文件 | 把任一 RAW 截掉后半（复制后用十六进制工具或 `fsutil file seteof`），打开 | | |
| 非 RAW 内容、RAW 扩展名 | 把一个文本文件改名为 `.ARW`，打开 | | |
| 未知 / 不支持的压缩 | 尼康 HE、CR3 等，后端设为 PhotoCraft | | |
| 取消 | 打开大样片的过程中立刻切到另一张 | | |
| 快速切图 | 在含多张 RAW 的文件夹里连续按上一张 / 下一张 | | |
| 连续批处理 | 批量导出 10 张以上，其中夹一张坏文件 | | |

每项之后检查：原片的修改时间和大小没有变；已有的编辑记录还在；系统盘临时目录和输出目录
里没有残留的中间文件（`.partial-` 开头或含 `.partial-` 的文件）。

诊断包、报告 JSON、截图放在一起，按样片编号命名，随记录提交。

## 6. 结论

| 样片 | 结论（通过 / 不通过 / 待测） | 是否建议加入默认名单 | 依据 |
|---|---|---|---|
| 1 | 待测 | | |
| 2 | 待测 | | |
| 3 | 待测 | | |
| 4 | 待测 | | |
| 5 | 待测 | | |

加入默认名单的条件：第 2、3、5 节在该机型和该压缩模式下通过，与 LibRaw 的差异已记录
且可接受，性能数据已提交。名单在 `engine/host/raw_backend.cpp` 的
`accepted_photocraft_raws()`，每条写明这里的记录位置；`spk_host_raw_photocraft` 里有一行
固定了名单内容，改名单时一并修改。
