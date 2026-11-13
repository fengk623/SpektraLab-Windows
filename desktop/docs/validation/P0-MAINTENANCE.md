# P0 维护版真机验证清单（1.3.4-beta.3）

基线：上游 `Windows_Linux` 的 `9ceca338bac8aa0bcd8f17aeeb324df9174bb15c`，桌面版本号 `1.3.4-beta.3`。

P0 的完成条件有两条：安装包与便携包同源；86.1 MP JPEG 可处理或受控拒绝，随后普通照片仍可渲染。下面每一项写一份 [测试记录](TEST-RECORD-TEMPLATE.md)。CI 已覆盖的部分只贴运行链接，不在真机上重做。

## 0. 每次运行先取的环境信息

以下命令在编写时没有在 Windows 上执行过，第一次使用时核对输出是否符合预期。

```powershell
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate
dxdiag /t "$env:TEMP\dxdiag.txt"
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' | Select-Object Tdr*
```

- 专用显存从 dxdiag 输出里读。`Win32_VideoController.AdapterRAM` 是 32 位字段，超过 4 GB 的显卡读数不可信。
- 最后一条没有返回任何 `Tdr*` 值，表示 TDR 是系统默认。GPU 01 要求用系统默认设置；有改动就先记录原值，不要为了让测试通过去调大超时。

## 1. 构建与验证

| 编号 | 步骤 | 通过条件 | 证据 |
|---|---|---|---|
| BUILD-01 | 在个人仓库对基线分支手动触发 **Desktop (Linux, Windows)** 工作流，不勾选 `publish_windows` | Linux、Windows 交叉构建、MSVC + CTest、前端检查、两个打包任务全部通过 | 运行链接；失败的检查逐条记录现象和修复提交 |
| BUILD-02 | 在有真实 Vulkan GPU 的 Windows 机器上，按 `desktop.yml` 中 `engine-windows-msvc` 任务的配置参数构建，然后运行完整 CTest，**不加** CI 用的 `-E` 排除 | CI 排除的 `spk_vulkan_*`、`spk_math_gate_*`、`spk_windows_c_abi`、`spk_windows_feature_boundaries` 在真实设备上通过 | `ctest --output-on-failure` 全文、测试总数、GPU 与驱动 |

CI 在没有 GPU 的 runner 上跳过 BUILD-02 列出的那几组测试，所以 CI 全绿不说明它们通过。

## 2. 安装与更新

在干净的 Windows x64 环境做：没有装过 SpektraLab，用户目录里没有旧的配置和缓存。

| 编号 | 步骤 | 通过条件 | 证据 |
|---|---|---|---|
| REL-01 | 下载发布页的五个文件到一个目录，运行 `node .github/scripts/windows-verify-release.mjs --dir <目录>` | 报告 `ok: true`；`tag` 以 `spektralab-windows-v` 开头；`commit` 是预期的源码提交 | 报告 JSON |
| REL-02 | 运行安装包，**首次启动前**再运行一次并加 `--installed <安装目录>` | 报告 `ok: true`，`installed_files_compared` 大于 0 | 报告 JSON。这个模式此前只有夹具覆盖，第一次真机结果连同任何误报一起记录 |
| INST-01 | 安装版首次启动，打开一张普通照片，调参，导出 | 启动无报错；预览、调参生效；导出文件可被其他软件打开，嵌入 ICC | 导出样张、诊断包 |
| INST-02 | 完整解压便携包到新目录，运行 `spektralab.exe`，重复 INST-01 | 同上。便携版的配置、缓存和日志仍写入用户目录，这是预期行为 | 导出样张、诊断包 |
| UPD-01 | 在已安装的旧测试版（如 beta.2）里打开设置，点“检查 Windows 更新” | 报告有更新，版本为最新的 Windows 预发布；“打开发布页面”进入 `spektralab-windows-v…` 的发布页，不是 macOS 发布页 | 截图 |
| UPD-02 | 在最新测试版里重复 | 报告没有更新的版本，显示的版本就是当前版本 | 截图 |
| UPD-03 | 发布 RC 或正式版之后，在 beta 客户端重复 | beta 之后是 rc，rc 之后是正式版；序号按数值比较（beta.10 高于 beta.9） | 截图 |
| GPU-A380 | 在 Intel Arc A380 上启动安装版并打开一张普通照片 | 启动并完成首张渲染 | 诊断包、驱动版本 |

UPD-03 的排序规则已有单元测试覆盖（`desktop/src/windows/updateCheck.test.ts`），真机只确认线上发布列表与测试用的数据形状一致。

## 3. GPU 01 大图与设备丢失

用 Issue #62 的原场景：8 GB 显存设备，86.1 MP JPEG，系统默认 TDR。

| 编号 | 步骤 | 通过条件 | 证据 |
|---|---|---|---|
| GPU-01 | 导入含该大图的文件夹，在缩略图仍在生成时打开并操作该大图。连续做三次，每次先确认缩略图确实还在生成 | 连续三次，每次都完成处理或给出可解释的受控拒绝 | 每次的结果、耗时、显存峰值；诊断包 |
| GPU-02 | 每次 GPU-01 之后，切换到一张普通照片，编辑并导出 | 普通照片正常渲染并导出 | 导出样张 |
| GPU-03 | 记录缩略图、预览、全尺寸渲染三者的调度顺序、各自耗时和显存峰值 | 有实测数据 | 开启“记录诊断”里的详细计时单独跑一轮取调度；计时会带来同步开销，耗时数字用关闭计时的那一轮 |
| GPU-04 | 对 102 MP RAF 和 IQ4 IIQ 重复 GPU-01、GPU-02 | 同 GPU-01、GPU-02 | 同上，另记驱动版本、适配器和显存 |

基线的已知状态：设备丢失后应用停止后续提交并提示重启（PR #64），没有自动恢复，也不能防止驱动复位。发生设备丢失时照实记录，这属于已知限制，不算本清单的新缺陷；GPU-02 在这种情况下记为未通过。

## 4. 维护回归

每项的通过条件以需求文档对应条目为准，这里只列要取的证据。

| 编号 | 范围 | 要留的证据 |
|---|---|---|
| COLOR-01 | Sony ARW 原图偏暗；松下 S5M2 有、无机内 LUT 的 RW2；富士 X-E5 无压缩 RAF、佳能 R6 Mark III Full RAW/C-RAW、索尼 A7 V ARW6 已测压缩样本。A7 V 无损 RAW 单独记为待验证 | 固定参数下的原图、预览、导出和直方图；样片的具体模式 |
| CACHE-01 | 显示预览缓存换盘：设置保存、容量上限、旧缓存清理、目标盘不可用、C 盘剩余空间偏低 | 显示预览与引擎临时文件各自的目录和峰值占用，分开记 |
| META-01 | 带 XMP 的 PNG 的拍摄参数（PR #101 不在 beta.3 内） | EXIF 优先、XMP 补齐缺失字段、日期戳使用拍摄日期三项各自的结果 |
