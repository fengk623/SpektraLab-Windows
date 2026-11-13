# Windows 验证记录

本目录放 Windows 维护版与 RAW 支持的验证材料：真机清单、测试记录模板和样片登记表。原片、导出样张和诊断包不进仓库，只登记来源和校验值。

| 文件 | 用途 |
|---|---|
| [P0-MAINTENANCE.md](P0-MAINTENANCE.md) | 1.3.4-beta.3 维护版的真机验证清单，逐项写明步骤、通过条件和要留的证据 |
| [TEST-RECORD-TEMPLATE.md](TEST-RECORD-TEMPLATE.md) | 单次验证记录的模板，一次运行一份 |
| [SAMPLES-TEMPLATE.csv](SAMPLES-TEMPLATE.csv) | 样片登记表的表头，逐件一行 |

## 三条口径

1. **支持**：每个宣称支持的机型与模式，至少一份真实原片完成探测、冲洗、调参和全分辨率导出，并复核输出尺寸、方向和色彩。能选中文件或显示内嵌预览不算支持。
2. **受控失败**：未支持的压缩、截断文件或资源不足时，错误提示说明原因，随后能继续处理下一张，原文件和已有编辑数据保持不变。
3. **证据对应一次构建**：记录里的构建提交必须来自 `windows-verify-release.mjs` 的报告（见 [WINDOWS-RELEASES.md](../WINDOWS-RELEASES.md)），不抄版本号。分支上的版本号只代表源码基线。

托管 runner 没有真实 Vulkan GPU。CI 全绿不能代替本目录里的真机项；反过来，真机记录也不重复 CI 已经证明的内容，只写 CI 链接。

## 样片登记字段

`SAMPLES-TEMPLATE.csv` 每件原片一行，文件名之外的字段含义如下。

| 字段 | 填写内容 |
|---|---|
| `sample_id` | 自定的稳定编号，测试记录用它引用样片 |
| `source` | 来源：用户授权、raw.pixls.us、DPReview 样片图库等，附链接或联系人 |
| `terms` | 使用条件原文或链接 |
| `downloadable` / `redistributable` | 分别填 `yes`、`no` 或 `unknown`。可下载不等于可再分发，两项分开确认 |
| `storage` | 受限样片所在的授权目录；可公开样片填下载地址 |
| `sha256` | 原片的 SHA-256。验证前后各算一次，确认原片未被改动 |
| `make` / `model` / `firmware` | 以文件 EXIF 为准，不以文件名或下载页说明为准 |
| `mode` | 实际压缩模式，例如 `HE*`、`HE`、`lossless`、`L16`、`L14`、`S14`。用 EXIF 和文件信息确认 |
| `bit_depth` / `width` / `height` | 位深与像素尺寸 |
| `expected` | 预期结果：`support`（完整流程通过）或 `controlled_failure`（明确拒绝并可继续） |
| `notes` | 关联的 issue、已知特殊情况 |

登记完成的表格与原片放在一起管理。含受限样片信息的登记表是否进仓库，由样片的使用条件决定。
