---
id: RFC-W002
title: spklib 辅助程序的 Windows 移植：平台层、Capture One for Windows 的路径、构建与验收
status: Review
owner: fengk623
created: 2026-10-28
updated: 2026-10-30
baseline: 368d1974943a4a4982339fd355a859cad74e69a4
prd: PRD-W001
tests:
  - vendor/spklib/windows/patches
  - .github/workflows/spklib-windows.yml
---

# RFC-W002：spklib 辅助程序的 Windows 移植

## 1. 摘要（Summary）

把 spklib（SpektraLab 2.0 的图库 SDK 与 `spklib-host` 辅助程序，MIT，JamesQiu2005/spektralab_library @ `afafee6`）移植到 Windows x64。改动集中在一个新的平台层 `src/platform.hpp`，macOS 与 Linux 上的行为逐字节不变；新增 Capture One for Windows 目录的读法（`ZWINROOT`、`\` 分隔、`ZPARENT = -1`）、Windows 文件锁下的快照、UTF-8 代码页清单、`build-windows.cmd`（llvm-mingw）与一个对真实目录的验收脚本。补丁、构建出的 `spklib-host.exe` 与验收记录放在本仓库 `vendor/spklib/windows/`，MR 由用户本人向 spektralab_library 提交。

## 2. 现状与动机（Context）

- `afafee6` 只能在 macOS 构建：`capture_one.cpp:65` 用 `st_mtimespec`，`library.cpp` 用 `gmtime_r`、`timegm`、`localtime_r`、`renamex_np`，`nfc()` 只在 Apple 上实现；路径运算假定根是 `/`（`join_path`、`is_prefix_dir`、`ensure_location`）；`c1::resolve` 只用 `ZMACROOT`。Linux 上除 `st_mtimespec` 外都能编译（2026-10-28 实测）。
- Capture One 16.6.4 for Windows 建的目录（2026-10-28，本机）：`ZWINROOT='C:\'`、`ZMACROOT=''`、`ZRELATIVEPATH='Users\Sizhi\Desktop\…\photos\Day1'`；`ZVERSIONINFO.ZFORMAT='16.6.4.3044 for Phase One Win'`；`ZPARENT=-1` 出现在根 ProjectCollection、全部图像、回收箱、在目录中与顶层文件夹上。
- 对应 PRD-W001 的 R1–R8。

## 3. 设计（Design）

### 3.1 平台层（`src/platform.hpp`）

| 函数 | macOS | Linux | Windows |
|---|---|---|---|
| `file_stat` | `stat`，`st_mtimespec` | `stat`，`st_mtim` | `GetFileAttributesExW`，FILETIME → Unix 纳秒 |
| `utc_tm` / `local_tm` / `utc_time` | `gmtime_r` / `localtime_r` / `timegm` | 同左 | MSVC：`gmtime_s` / `localtime_s`；MinGW：CRT 的线程局部 `gmtime` / `localtime`；`_mkgmtime` |
| `rename_no_replace` | `renamex_np(RENAME_EXCL)` | `rename(2)`（会覆盖，与改动前相同；Linux 不发布） | `MoveFileExW`，不带 `REPLACE_EXISTING` 与 `COPY_ALLOWED` |
| `nfc` | CoreFoundation | 恒等（与改动前相同） | `NormalizeString(NormalizationC)` |
| `path_string` | `path::string()` | 同左 | `path::generic_string()`（`/` 分隔） |

### 3.2 路径的拼写

spklib 内部与对外都用 `/` 分隔的 UTF-8 字符串。Windows 上的根是 `C:/` 或 `//server/share/`：`root_length`、`strip_trailing_separators` 知道这一点，`join_path`、`is_prefix_dir`、`ensure_location`、`c1::resolve` 用它们。POSIX 上这些函数的结果与改动前完全相同（根恒为 `/`）。

### 3.3 Capture One for Windows

- `c1::platform_root`：Windows 上取 `ZWINROOT`（`\` 换成 `/`，`C:` 补成 `C:/`），为空时退回 `ZMACROOT`（Mac 建的目录，在 Windows 上就是离线）；其他平台取 `ZMACROOT`，与以前相同。`read()` 把它存进 `PathLocation::root`，同步、拷贝、`resolve` 都用它。
- `ZRELATIVEPATH` 在 Windows 上把 `\` 当分隔符。
- `ZPARENT = -1`：与“父行已不存在”一样放置（非文件夹挂在根下，文件夹挂在路径上最近的上级文件夹下），但根永远不是自己的父级，`-1` 也不计入 `parents_repaired`。

### 3.4 快照（只读，永远不碰原文件）

- 只读快照用 `file:///C:/…?mode=ro&immutable=1`（UNC 为 `file:////server/…`）。
- Windows 的文件锁是强制的：Capture One 持有 SQLite 的 RESERVED / PENDING / EXCLUSIVE 锁时，读锁字节页（1 GiB 处那一页）会失败。快照复制用 `CreateFileW`（共享读、写、删除），那一页写零、不读；SQLite 从不使用该页。其他平台照旧逐字节复制。
- 复制得到的字节数少于原文件大小时，在所有平台上都按“复制时目录变了”重试，而不是交给 `quick_check`。

### 3.5 进程

- 宿主把 stdin/stdout 设为二进制模式，回复以 `\n` 结尾。
- 清单 `host/spklib.manifest`：`activeCodePage` UTF-8（argv、`std::filesystem::path`、`std::ifstream` 都按 UTF-8 解释）、`longPathAware`。`spklib_create` 在代码页不是 UTF-8 时拒绝启动。

### 3.6 构建

`build-windows.cmd`：llvm-mingw 的 clang / llvm-ar / llvm-windres，`-static`，SQLite 合并源码编进库（`third_party/sqlite/fetch.py` 按 sqlite.org 公布的 SHA3-256 校验，不入库）。`release`：`-O2 -DNDEBUG -s`、`--no-insert-timestamp`、`-ffile-prefix-map`（源码目录与工具链目录），版本串 `0.3.2+<commit>`。

## 4. 接口与契约（Contracts）

- spklib 的 C ABI、方法表、错误码、库文件格式都不变：`abi` 1、`schema` 3。版本 0.3.2。
- Windows 上返回的路径用 `/` 分隔：写进 spklib `API-SPEC.md`。
- 本仓库的接口（`params_delta`、宿主协议、C ABI `spk_engine.h`）不涉及。

## 5. 影响评估（Impact）

- **许可**：见 PRD-W001 §5。
- **体积**：静态链接后的 `spklib-host.exe` 大小在验收记录里给出。
- **安全**：辅助程序不联网；构建时只从 sqlite.org 取一个按哈希校验的文件；工具链按 SHA-256 校验。
- **macOS**：`platform.hpp` 的 Apple 分支保留原实现；新增的“复制字节数不足即重试”在 macOS 上也生效（以前会由 `quick_check` 报错）。
- **用户文件**：测试与验收只写 `_local_spklib_validation`；spklib 自己的测试与快照会创建并清理它们自己的临时目录（在指定的 `TMP` 下）。

## 6. 测试计划（Test plan）

### 6.1 自动化

- spklib `tests/tests.cpp` 全部用例在 Linux（g++ 13）与 Windows（llvm-mingw）上通过；新增 `platform_port` 组：路径根与分隔符、两个平台的 `platform_root`/`resolve`、锁字节页复制（Windows 上在真实的 `LockFileEx` 下）、`rename_no_replace`、非 ASCII 目录下的目录、`ZPARENT -1`（先红后绿：旧代码失败 3 处）。
- CI `.github/workflows/spklib-windows.yml`：在 windows-latest 上把补丁应用到 `afafee6`、构建、跑测试，并把 release 构建与入库的 exe 逐字节比较。

### 6.2 人工验收（真实 Capture One）

`tests/c1_acceptance.py` 与 spklib-tests 的真实目录一轮，对 Capture One 16.6.4 for Windows 建的测试目录与测试会话运行；Capture One 关闭与打开各一次。测试数据、预检与结果写在 `vendor/spklib/windows/ACCEPTANCE.md`。`c1_acceptance.py` 逐项对照独立读取：probe 计数、每张图的路径与文件存在、废纸篓、每个变体的星级与色标、每个相册的图片与所在的相册组、每个文件夹的路径、名称、图片与上下级，以及前后文件清点。会话只做 probe。

## 7. 备选方案（Alternatives）

- **在 Windows 上把路径保持为 `\`**：要在所有字符串运算里区分两种分隔符，macOS 的代码路径也要改。否决。
- **安装 Visual Studio Build Tools**：要改动系统（安装目录、注册表），本机不允许。用便携的 llvm-mingw。MSVC 构建可以以后在 CI 加一个任务。
- **把 SQLite 合并源码放进 spklib 仓库**：约 9 MB 的文件进 MR，审阅困难。改为按哈希下载；离线构建可用 `fetch.py --zip`。
- **用 `fs::copy_file` 做快照**：Windows 上 `CopyFile` 打不开 Capture One 以写方式打开的文件，也读不了被锁的页。否决。

## 8. 发布与迁移（Rollout）

1. 本仓库 `windows2`：`vendor/spklib/windows/`（补丁、`BUILD-INFO.env`、说明与验收记录；验收通过后再放入 exe 与 `.sha256`）与 CI。
2. 用户审核后，由用户本人把补丁提交为 spektralab_library 的 MR。
3. 图库接入桌面应用另立 PRD。

## 9. 决策记录（Decision log）

- 2026-10-28 · Windows 上路径统一用 `/` · 见 §7。
- 2026-10-28 · `ZPARENT = -1` 按“没有父行”放置，不算修复 · 实测数据（§2）。
- 2026-10-28 · 锁字节页写零 · SQLite 不使用该页；Windows 锁是强制的。
- 2026-10-30 · spklib 文档随补丁更新（0005：README、API-SPEC 0.3.2、CAPTURE-ONE-CATALOG 的 Windows 一节、SESSION-DESIGN §6 的初步答案） · MR 要自带说明，审阅者不必读本仓库。

## 10. 待决问题（Open questions）

- 仓库负责人：0.3.2 的版本号；SQLite 下载还是入库；是否另加 MSVC 构建。
- Capture One 会话的完整读取（spklib `SESSION-DESIGN.md` §6）。
