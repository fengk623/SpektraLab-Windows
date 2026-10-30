# spklib Windows 版验收记录

PRD-W001 §6 的验收项，逐项写明依据。本文件随每次验收运行更新；“待运行”的项还没有证据，不算通过。

**先说清楚一件事**：`capture_one.probe` 成功只说明数据库被识别（格式、计数、能取快照），不等于完整读取。目录（catalog）只有在 `tests/c1_acceptance.py` 的全部检查都通过时才算“完整读取通过”：每张图的路径、文件存在、废纸篓标记、每个变体的星级与色标、每个相册的图片与位置、每个文件夹的路径、名称、图片与上下级，并且 Capture One 的文件前后一致。会话（session）目前**只做 probe**：`capture_one.link` 对会话回答 `unsupported`，完整读取要等 spklib `docs/SESSION-DESIGN.md` §6 的核实与实现。

## 状态

| 项 | 内容 | 状态 | 依据 |
|---|---|---|---|
| A1 | 本机 `build-windows.cmd test`，spklib-tests 全部通过 | 通过（补丁 0001–0005） | 开发机 2026-10-30 12:50：`spklib 0.3.2+d203db8a5ff8`，**561 passed, 0 failed**（与 CI 相同） |
| A2 | CI：补丁应用到 `afafee6`，构建、测试、release 构建 | 通过（`77aea7b`，补丁 0001–0005）；逐字节比较待 exe 入库 | run 41087191443（windows-latest）：`git am` 5 个补丁、llvm-mingw 与 SQLite 校验通过，spklib-tests **561 passed, 0 failed**；release `spklib-host.exe` 3,009,536 字节，SHA-256 `1c03400e81ae287077393ad5fbbc7aaf9a097d2664ff1a0935be16af7b204800`（`SPKLIB_COMMIT` d203db8a5ff8） |
| A3 | `c1_acceptance.py`，Capture One 关闭，对测试目录与会话 | 通过 | 开发机 2026-10-30 12:52，release `spklib-host.exe`：**36 passed, 0 failed**（见运行记录 1） |
| A4 | spklib-tests 真实目录一轮（`SPKLIB_C1_CATALOG`） | 未通过 → 已修，待重跑 | 621 passed, 3 failed：3 处都是测试自己的独立读取有误（见运行记录 1），spklib 的结果与 Capture One 一致。修正在 spklib `ddb470c`，待重跑 |
| A5 | Capture One 打开测试目录时再跑一次 | 待运行 | 文件清点的差异如实记录 |
| A6 | `ZPARENT -1` 先红后绿 | 通过 | 旧代码失败 3 处，新代码通过（补丁 0002 的提交说明） |

## 测试数据（Capture One 16.6.4.3044 for Windows，2026-10-28/30）

全部在开发机的 `C:\Users\<user>\Desktop\SpektraLab-Windows_Linux\_local_spklib_validation\` 内。照片是合成的（纯色条纹，相机型号 `Synthetic Cam 1`），不含任何个人照片。

**测试目录 `SpkProbe Catalog 测试`**（文件夹形式，内含 `.cocatalogdb`，`journal_mode=delete`）：6 张图，引用在原位置（未复制进目录）。

| 文件 | 文件夹 | 星级 | 色标 | 相册 |
|---|---|---|---|---|
| `A_0001.jpg` | `photos\Day1` | 5 | 红（1） | Picks；Trips › 测试相册 |
| `A_0002.jpg` | `photos\Day1` | 4 | 蓝（5） | Picks |
| `A_0003.tif` | `photos\Day1` | 3 | 绿（4） | Trips › 测试相册 |
| `测试 0004.jpg` | `photos\Day2 子文件夹` | 2 | 无 | Trips › 测试相册 |
| `B_0005.png` | `photos\Day2 子文件夹` | 1 | 黄（3） | — |
| `B_0006.jpg` | `photos\Day2 子文件夹` | 0 | 无 | — |

另有 Capture One 自动建的导入相册（6 张）。“Trips”是相册组（album group），“测试相册”在组内。文件夹树：`C:\` › `Users` › … › `photos` › `Day1` / `Day2 子文件夹`（数据库里每一级一行，顶层 `ZPARENT = -1`）。

**测试会话 `SpkProbe Session 测试`**（空白模板）：`Day1` 的 3 张图复制进 `Capture`。

| 文件 | 星级 | 色标 | 相册 |
|---|---|---|---|
| `A_0001.jpg` | 2 | 无 | — |
| `A_0002.jpg` | 0 | 无 | — |
| `A_0003.tif` | 5 | 粉红（6） | Session Picks |

## 预检：Linux 上对两份数据库的字节副本（2026-10-30）

不是验收，只为在开发机运行前找出脚本与读取逻辑的问题。用 Linux 版 spklib-host（与补丁同源）对 `.cocatalogdb` 与 `.cosessiondb` 的副本运行扩充后的 `c1_acceptance.py`：

- probe 计数与独立读取一致：目录 images 6、variants 6、locations 9、collections 19、trashed 0、inside_catalog 0、rated 5、color_tagged 4；会话 images 3、variants 3、rated 2、color_tagged 1；会话 link 回答 `unsupported`。
- 每个变体的星级与色标、3 个相册各自的图片与位置、文件夹的名称与图片（盘符那一级之外的 8 个；Linux 上盘符那一级没有路径）：一致。
- 34 项通过、2 项不通过，这 2 项都取决于平台的根：Linux 上读 Windows 建的目录只能用 `ZMACROOT`（为空），路径本来就解析不出来。把独立读取切到 Windows 规则（`ZWINROOT`）后，文件夹树与 spklib 的结果一致。真正的路径与文件存在检查只能在 Windows 上做。

## 在开发机上运行（A1、A3、A4、A5）

`_local_spklib_validation` 里的脚本只写这个文件夹，不安装任何东西：

1. 退出 Capture One（目录和会话两个窗口都关）。
2. 双击 `50-run-all.cmd`：依次 `10` 下载并校验 llvm-mingw 与 SQLite、`20` 构建并跑测试、`30` release 构建、`40` 验收（检测到 Capture One 未运行，记为 `c1-closed`）。结束时窗口里显示摘要，也写在 `50-run-all.log`。
3. 在 Capture One 里重新打开测试目录，再双击 `40-acceptance.cmd`（记为 `c1-open`）。

结果在 `20-build-and-test.log`、`30-release.log`、`40-acceptance-c1-closed.log`、`40-acceptance-c1-open.log` 与 `acceptance\<label>\`（`report.json`、`spklib-tests-real-catalog.txt`）。

## 运行记录

### 1. 开发机，Capture One 关闭（`c1-closed`），2026-10-30 12:50–12:52 CDT

`50-run-all.cmd`，spklib `d203db8`（补丁 0001–0005），llvm-mingw 20261006（SHA-256 校验通过），SQLite 3.54.1（SHA3-256 校验通过）。

- **构建与测试（A1）**：561 passed, 0 failed。
- **release**：`spklib-host.exe` 3,009,536 字节，SHA-256 `3956ba4ab7502ad28d912d4bb7eee30f3b055e8c088a8e01a6c92f8aaae54c4d`。与 CI（`1c03400e…`）大小相同、**字节不同**：同样的源码与工具链没有给出同样的字节，正在查（CI 上加了第二次构建与逐字节定位，见 `.github/scripts/pe_diff.py`）。查清之前不入库 exe。二进制里没有本机路径（`strings` 只见到 llvm-mingw 预编译库自带的 `/home/runner/...`），PE 头与资源目录的时间戳都是 0。
- **验收（A3）**，`c1_acceptance.py` 经 release `spklib-host.exe`：36 passed, 0 failed。
  - 目录：probe 计数（images 6、variants 6、locations 9、collections 19、trashed 0、inside_catalog 0、rated 5、color_tagged 4）与独立读取一致；link 后 6 张图的路径全部是 `C:/Users/<user>/Desktop/SpektraLab-Windows_Linux/_local_spklib_validation/photos/Day1/…` 与 `…/photos/Day2 子文件夹/…`，文件都存在；每张图的星级、色标与上表一致；3 个相册的图片与位置一致（Picks 在顶层，测试相册在 Trips 下，导入相册在“最近导入”下）；9 个文件夹的路径、名称（`C:/`、`Users`…`Day1`、`Day2 子文件夹`）、图片（Day1 3 张、Day2 子文件夹 3 张）与上下级一致；一个根，没有自己是自己父级的汇集。
  - 会话：probe 识别为 session，计数 images 3、variants 3、rated 2、color_tagged 1 与独立读取一致；link 回答 `unsupported`。
  - **不修改原库**：目录文件夹 20 个文件、会话文件夹 16 个文件，运行前后的大小、修改时间与 SHA-256 完全相同。
- **真实目录一轮（A4）**：621 passed, 3 failed。“the Capture One package is exactly as it was” 通过。3 处失败都在测试的独立读取：
  1. 与 2. 文件夹的图片数：测试从 `ZIMAGEINCOLLECTION` 数文件夹成员。Mac 版目录里有这些行；**Capture One for Windows 不为文件夹写这些行**，于是测试读到 0，而 spklib 按位置数出 3（与 Capture One 显示的一致）。修正：文件夹的图片按位置数（spklib `ddb470c`）。
  3. facets：测试写死了仓库负责人的相机型号 `NIKON Z 8`，在别的目录里查不到。修正：取该目录自己最常见的相机型号。
- 另发现：`spklib-tests` 在临时目录里留下一个空的 `spklib-probe-*` 文件夹，来自“目录正在写入时 probe 被拒”的用例：快照失败时 probe 没有删掉自己的临时文件夹（上游原有）。修正并加了先红后绿的测试（spklib `c474dff`）。
