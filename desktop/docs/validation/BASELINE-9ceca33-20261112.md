# 基线 9ceca33 自动化检查记录

日期：2026-11-12。

## 结论

基线 `9ceca338bac8aa0bcd8f17aeeb324df9174bb15c`（桌面版本号 `1.3.4-beta.3`）上，能在编写环境里运行的三组检查全部通过。引擎构建、CTest、前端检查和打包没有在这个环境里运行，原因见下表；它们的基线结果要从 CI 运行取得（[P0-MAINTENANCE.md](P0-MAINTENANCE.md) 的 BUILD-01）。

本记录不含任何真机、GPU、相机样片或性能结果。

## 环境

Linux x64 云端工作区：Node 22.23.4、Python 3.13.18、CMake 3.28.3、Ninja 1.11.1、GCC 13.3.0。没有 glslang、meson、MinGW-w64、Vulkan 设备和 Windows；npm registry 不可达，`npm ci` 无法完成。

## 已运行

| 检查 | 命令 | 结果 |
|---|---|---|
| Windows 发布不变量 | `node --test .github/scripts/windows-release.test.mjs` | 4 项通过，0 失败 |
| 依赖准备回归 | `python3 -m unittest discover -s engine/tests -p 'test_*codec*.py'` | 6 项通过，0 失败 |
| 更新选择器 | `desktop/src/windows/updateCheck.test.ts` 的 7 个用例，经 node:test 替身运行（不是 vitest） | 7 项通过，0 失败 |

更新选择器另用冻结发布的五个附件做了探测：beta → rc → 正式版的顺序、序号按数值比较、稳定版客户端不接收预发布、beta 客户端不把旧的 macOS 混合发布当作 Windows 更新，结果都符合 `desktop/docs/releases/beta.3.md` 的描述。发现一处与附件顺序有关的问题：附件按名称排序时，设置页显示的是便携包而不是安装包。修复在分支 `p0/update-installer-entry`。

## 未运行

| 检查 | 原因 |
|---|---|
| `engine/build-linux.sh --test`（Linux 引擎构建、CTest、host smoke） | 缺 glslang、meson 和 Vulkan 设备 |
| `engine/build-windows-cross.sh` 与 wine 下的 host smoke | 缺 MinGW-w64 和 wine |
| MSVC 构建与 CTest | 需要 Windows |
| `npm run check`（tsc、eslint、vitest）、Playwright、`cargo test` | 依赖无法安装 |
| 安装包与便携包打包、静默安装检查 | 需要 Windows runner |
| 线上发布列表的更新检查 | GitHub API 在此环境不可读；见 UPD-01、UPD-02 |

## 发布溯源的代码审阅

`.github/scripts/windows-release.mjs` 在发布时核对：六处版本号一致；工作区干净且等于触发提交；便携包内 `BUILD-MANIFEST.json` 的提交和版本；两个可执行文件的哈希；已有标签必须指向同一提交；草稿附件的摘要逐个比对后才公开。源码 ZIP 由 `git archive <commit>` 生成。

发布侧没有发现缺口。缺的是下载侧的核对手段，由 `windows-verify-release.mjs` 补上（分支 `p0/release-verification`）。

一个留给维护者决定的问题：稳定版客户端仍把带 Windows 附件的旧 macOS 混合发布（`spektralab-v…`）视为候选。Windows 独立线出正式版之后，如果某个更高版本号的 macOS 发布页还挂着滚动的 Windows 安装包，稳定版 Windows 客户端会被提示有更新。beta 客户端不受影响。本次没有改动这一行为。
