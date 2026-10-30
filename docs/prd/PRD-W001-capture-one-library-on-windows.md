---
id: PRD-W001
title: Windows 上读取 Capture One 图库：spklib 辅助程序的 Windows 版
status: Review
owner: fengk623
created: 2026-10-28
updated: 2026-10-30
baseline: 368d1974943a4a4982339fd355a859cad74e69a4
rfcs: [RFC-W002]
---

# PRD-W001：Windows 上读取 Capture One 图库：spklib 辅助程序的 Windows 版

## 1. 背景与现状（Background）

- macOS 版的 2.0 图库（目录、会话、读取 Capture One 目录）全部在一个辅助程序 `spklib-host` 里：`main` @ `fd891c6` 附带它的二进制 `vendor/spklib/macos/spklib-host`（SHA-256 `67127692…40b9`），契约写在 `API-SPEC-library-helper.md`（“The binary is in the repository. The app does not launch it yet”）。源码在 JamesQiu2005/spektralab_library（MIT），`afafee6`（0.3.1），只有 macOS 构建（`build.sh`：Xcode clang、系统 SQLite、CoreFoundation）。
- 本分支（基线 `368d197`，即 evanfan005-bit 的 `Windows_Linux`）没有任何图库代码，也没有 Windows 版的辅助程序。
- 仓库负责人（邱瀚泽，2026-10-28）的要求：在 Windows 上用 Capture One 建一个测试目录和一个测试会话，导入照片，设置星级、色标和相册；编译 Windows 版 spklib 与 spklib-host；用 `capture_one.probe` 检测两种数据库；用相应接口检查照片数量、路径、评分和文件夹结构；确认不修改 Capture One 的原始数据库；测试通过后提交 MR。probe 成功只说明初步识别，不等于完整读取。
- 在 Windows 上实测（Capture One 16.6.4 for Windows 建的目录，2026-10-28）：`ZPATHLOCATION.ZWINROOT` 是 `C:\`、`ZMACROOT` 是空串、`ZRELATIVEPATH` 用 `\`；顶层汇集的 `ZPARENT` 是 `-1`。按 macOS 的读法，这样的目录路径全部解析错，根汇集会成为自己的父级。

## 2. 目标与非目标（Goals）

目标：
- spklib 与 spklib-host 在 Windows x64 上构建，单个静态 exe，可从固定版本的工具链复现。
- 读取 Capture One for Windows 写的目录：每张图的路径、星级、色标、废纸篓标记、相册与文件夹树，与数据库一致。
- 识别 Capture One 会话数据库（probe），并如实说明它还不是完整读取。
- 任何操作都不改动 Capture One 的目录包与会话文件夹。

非目标：
- 把图库接进 Windows 桌面应用（Tauri 前端、宿主进程管理、存储位置），另写 PRD。
- 完整读取 Capture One 会话：上游设计（spklib `docs/SESSION-DESIGN.md` §6）要求先核实会话结构与 `.cos` 文件，在那之前 `capture_one.link` 对会话回答 `unsupported`。
- 写回 Capture One。

## 3. 需求（Requirements）

- R1 `build-windows.cmd` 用 llvm-mingw 20261006（ucrt，x86_64，SHA-256 校验）与 SQLite 3.54.1（SHA3-256 校验）构建；`release` 输出不带时间戳、不含本机路径，同样的源码与工具链得到同样的字节。
- R2 spklib 已有的全部测试在 Windows 上通过；Windows 特有的行为各有测试。
- R3 Capture One for Windows 的根与路径：`ZWINROOT`（为空时用 `ZMACROOT`）、`\` 分隔的相对路径、`ZPARENT = -1`。
- R4 非 ASCII 路径（中文文件夹与文件名）可用；路径比较做 Unicode NFC。
- R5 Capture One 打开着目录时也能取快照（共享读、SQLite 锁字节页）。
- R6 不修改 Capture One 的任何文件：前后各做一次全部文件的大小、修改时间、SHA-256 清点，结果一致。
- R7 会话：`capture_one.probe` 报告计数，与独立读取一致；`capture_one.link` 回答 `unsupported`。
- R8 stdio 协议：每条回复以 `\n` 结尾（不是 `\r\n`），与 macOS 版相同。

## 4. Windows 平台习惯（Windows conventions）

不涉及界面：这是命令行辅助程序。与平台相关的约定：返回的路径用 `/` 分隔（`C:/Users/…`），Windows 的文件 API 都接受；应用显示给用户之前可以换成 `\`。程序靠清单（manifest）使用 UTF-8 代码页，需要 Windows 10 1903 或更新。

## 5. 约束（Constraints）

- Capture One 只读，永远经快照读取（spklib `docs/DESIGN.md` “Capture One, one way”）。
- 测试与验收不写用户的文件夹：临时文件、镜像库、副本都在 `_local_spklib_validation` 内（`TMP`、`TEMP` 指向那里）；本机上禁止删除或修改 `C:\Users\Sizhi\Desktop\SpektraLab-Windows_Linux` 之外的已有文件（用户指示，2026-10-28 22:22 CDT）。
- 许可：spklib MIT；SQLite 公有领域；静态链接的 libc++、libunwind（Apache-2.0 WITH LLVM-exception）与 mingw-w64 运行库（ZPL / MIT 类）允许再分发；辅助程序是单独的程序，与 GPL-3.0-or-later 的应用之间通过管道通信（参照 `API-SPEC-library-helper.md` 的做法）。

## 6. 验收标准（Acceptance）

- A1 本机 `build-windows.cmd test`：spklib-tests 全部通过（记录用例数）。
- A2 CI `spklib-windows.yml`（windows-latest）：补丁应用到 `afafee6` 后构建、测试通过，release 构建与入库的 exe 逐字节一致。
- A3 `tests/c1_acceptance.py`，在 Capture One 关闭时对测试目录与测试会话运行：全部检查通过（probe 计数、每张图路径、文件存在、废纸篓、每个变体的星级与色标、每个相册的图片（数量与具体哪几张）及其所在的相册组、每个文件夹的路径、名称、图片与上下级、目录与会话文件的前后清点一致）。记录在 `vendor/spklib/windows/ACCEPTANCE.md`。
- A4 spklib-tests 的真实目录一轮（`SPKLIB_C1_CATALOG`）：全部通过，包括“the Capture One package is exactly as it was”。
- A5 Capture One 打开测试目录时再跑一次 probe 与读取：成功；文件清点的差异如实记录（Capture One 自己会写）。
- A6 先红后绿：`ZPARENT -1` 的测试在旧代码上失败（3 处），在新代码上通过。

## 7. 测量计划（Measurements）

只记录事实，不作性能声明：测试目录的 link 耗时（spklib 自报的 `ms`）、release exe 的大小与 SHA-256、本机与 CI 构建结果是否逐字节一致。

## 8. 决策记录（Decisions）

- 2026-10-28 · 工具链用 llvm-mingw 便携版，放在 `_local_spklib_validation` 内，不安装 · fengk623 · 本机没有任何 C++ 编译器，安装 Visual Studio 会改动该文件夹之外的系统。
- 2026-10-28 · Windows 上路径统一用 `/` · Claude（待审核） · 一套字符串运算覆盖三个平台，macOS/Linux 的行为逐字节不变。
- 2026-10-30 · 验收脚本加查相册的具体图片与位置、文件夹的路径、名称与图片 · Claude（待审核） · 需求里的“文件夹结构”与相册不只是计数。

## 9. 待决问题（Open questions）

- 仓库负责人：Windows 版本号 0.3.2 是否合适；SQLite 是按固定版本下载（现做法）还是直接放进仓库。
- 应用接入时：Windows 上镜像库的存放位置（建议 `%LOCALAPPDATA%\SpektraLab\Libraries`），在接入的 PRD 里定。

## 10. 范围之外（Out of scope）

桌面应用的图库界面；Capture One 会话的完整读取与 `.cos` 解析；Linux 版发布（Linux 上 `rename` 会覆盖、NFC 为恒等，测试里如实标为跳过）。
