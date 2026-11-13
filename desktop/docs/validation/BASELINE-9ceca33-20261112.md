# 基线 9ceca33 自动化检查记录

日期：2026-11-12。

## 结论

基线 `9ceca338bac8aa0bcd8f17aeeb324df9174bb15c`（桌面版本号 `1.3.4-beta.3`）在个人仓库的 CI 上全部通过（BUILD-01），没有失败的检查需要修复。编写环境里能运行的三组检查也全部通过。

本记录不含任何真机、GPU、相机样片或性能结果。CI 的 Windows runner 没有 Vulkan 设备，Linux 上用的是 CPU 实现的 llvmpipe。

## CI 运行（BUILD-01）

[运行 43272720556](https://github.com/fengk623/SpektraLab-Windows/actions/runs/43272720556)：`workflow_dispatch`，分支 `baseline/1.3.4-beta.3`，未勾选 `publish_windows`，2026-11-12 17:45–18:08 UTC。

| 任务 | 结果 | 日志中的依据 |
|---|---|---|
| Engine + host (Linux, lavapipe) | 通过 | CTest 46 项全部通过；设备为 llvmpipe (LLVM 20.1.2) |
| Engine + host (Windows x64, cross-built) | 通过 | 依赖准备测试 6 项通过；exe 只导入系统 DLL；wine 下 host smoke 0 失败 |
| Engine (Windows x64, MSVC, CTest) | 通过 | 注册 36 项，运行 11 项全部通过；其余 25 项是按 GPU 排除的 |
| Frontend checks | 通过 | vitest 463 通过、1 跳过（67 个文件）；发布不变量 4 项；cargo test 24 通过、1 忽略；Playwright 61 通过 |
| Package (Linux .deb + AppImage) | 通过 | |
| Package (Windows NSIS installer) | 通过 | 安装包 37.3 MB；静默安装后应用、宿主和引擎资源就位；已安装宿主与便携包宿主都应答 `hello`，报告 `0.1.0+9ceca33`；便携包由已安装文件组装，957 个文件 |
| Publish the Windows pre-release | 跳过 | 符合预期：未请求发布，且不在 `Windows_Linux` 分支 |

两点要带进真机验证：

- MSVC 构建的 36 项测试里有 25 项没有在 CI 上运行。它们只能在有真实 Vulkan GPU 的 Windows 机器上跑（[P0-MAINTENANCE.md](P0-MAINTENANCE.md) 的 BUILD-02）。
- Windows runner 上宿主报告 `vkCreateInstance failed (VkResult -9)`，这是没有显卡驱动时的预期提示。安装检查证明的是文件就位和协议应答，不是渲染。

该运行的 `spektralab-windows-x64` 产物含安装包和便携包，来自基线提交，2026-11-26 过期。编写环境的网络策略不允许下载它，所以没有用它核对便携包清单。

## 环境

Linux x64 云端工作区：Node 22.23.4、Python 3.13.18、CMake 3.28.3、Ninja 1.11.1、GCC 13.3.0。没有 glslang、meson、MinGW-w64、Vulkan 设备和 Windows；npm registry 不可达，`npm ci` 无法完成。

## 编写环境里已运行

| 检查 | 命令 | 结果 |
|---|---|---|
| Windows 发布不变量 | `node --test .github/scripts/windows-release.test.mjs` | 4 项通过，0 失败 |
| 依赖准备回归 | `python3 -m unittest discover -s engine/tests -p 'test_*codec*.py'` | 6 项通过，0 失败 |
| 更新选择器 | `desktop/src/windows/updateCheck.test.ts` 的 7 个用例，经 node:test 替身运行（不是 vitest） | 7 项通过，0 失败 |

更新选择器另用冻结发布的五个附件做了探测：beta → rc → 正式版的顺序、序号按数值比较、稳定版客户端不接收预发布、beta 客户端不把旧的 macOS 混合发布当作 Windows 更新，结果都符合 `desktop/docs/releases/beta.3.md` 的描述。发现一处与附件顺序有关的问题：附件按名称排序时，设置页显示的是便携包而不是安装包。修复在分支 `p0/update-installer-entry`。

## 编写环境里未运行

下列检查在编写环境里无法运行，结果以上面的 CI 运行为准。

| 检查 | 编写环境缺什么 |
|---|---|
| `engine/build-linux.sh --test` | glslang、meson 和 Vulkan 设备 |
| `engine/build-windows-cross.sh` 与 wine 下的 host smoke | MinGW-w64 和 wine |
| MSVC 构建与 CTest | Windows |
| `npm run check`、Playwright、`cargo test` | 依赖无法安装 |
| 安装包与便携包打包、静默安装检查 | Windows runner |

仍然没有任何地方运行过的：线上发布列表的更新检查（GitHub API 在编写环境不可读；见 UPD-01、UPD-02）。

## 发布溯源的代码审阅

`.github/scripts/windows-release.mjs` 在发布时核对：六处版本号一致；工作区干净且等于触发提交；便携包内 `BUILD-MANIFEST.json` 的提交和版本；两个可执行文件的哈希；已有标签必须指向同一提交；草稿附件的摘要逐个比对后才公开。源码 ZIP 由 `git archive <commit>` 生成。

发布侧没有发现缺口。缺的是下载侧的核对手段，由 `windows-verify-release.mjs` 补上（分支 `p0/release-verification`）。

一个留给维护者决定的问题：稳定版客户端仍把带 Windows 附件的旧 macOS 混合发布（`spektralab-v…`）视为候选。Windows 独立线出正式版之后，如果某个更高版本号的 macOS 发布页还挂着滚动的 Windows 安装包，稳定版 Windows 客户端会被提示有更新。beta 客户端不受影响。本次没有改动这一行为。
