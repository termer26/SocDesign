# 3DIC 多目标全局布线比赛求解器

`contest_router 1.2` 是 C++17 命令行全局布线求解器。输入 LEF（工艺、单元几何）、DEF（布局、引脚、网络、网格）和热图，输出 `.guide`（建议经过的布线区域）与本地 JSON 报告。`learning_router` 是另一个教学程序，输出 DEF，使用方法见 README。

2026-10-14 实测：新公开用例 **open_3d_case001 的 3152 条网络全部布通，比赛评分器 PASS，657 个 TSV**。这是全局布线基线，不包含综合、布局、详细布线和 DRC 签核。

2026-10-15 更新：**case003 的 151611 条网络、case005 的 179700 条网络均全部布通，比赛评分器 PASS**，分别使用 22574 和 39542 个 TSV。case005 本次重新布线的 guide 与此前完整结果 SHA256 一致，正式评分加权汇总为 **44.701731**；此前 OOM 和中断的记录保留在旧目录。

## 编译和运行 case001

在 Windows PowerShell 中执行：

```powershell
cd E:\tmp\EDA
cmake -S cpp_router -B cpp_router/build-contest -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build cpp_router/build-contest --parallel 4
ctest --test-dir cpp_router/build-contest --output-on-failure

.\cpp_router\build-contest\contest_router.exe `
  --lef .\case\open_3d_case001\open_3d_case001.input.lef `
  --def .\case\open_3d_case001\open_3d_case001.input.def `
  --thermal .\case\open_3d_case001\open_3d_case001.thermal.input.json `
  --out .\cpp_router\output\case001-adaptation\case001-final-initial.guide `
  --report .\cpp_router\output\case001-adaptation\case001-final-initial.json `
  --iterations 0 --time-limit 180
```

前三条 CMake 命令分别是“配置编译”“生成可执行文件”“运行测试”。后面的命令才是用可执行文件布线。`--iterations 0` 表示只做初布；本次增加两轮重布并未改善内部目标，最终恢复的 guide 与初布完全相同，所以复现本次结果只需初布。

输出目录自动创建，重复运行会替换指定输出。Guide 在 Windows/Linux 上均使用 LF 换行。程序先写临时文件、回读检查，再逐个替换 guide 和报告；应同时核对退出码和报告，一次中断不保证两个文件一起发布。输入路径与输出路径冲突会被拒绝。

Linux 构建只需要 g++，本机使用 WSL `ubun2`、Ubuntu 22.04、g++ 11.4.0。从 PowerShell 执行：

```powershell
wsl -d ubun2 -- bash /mnt/e/tmp/EDA/cpp_router/build_contest.sh
wsl -d ubun2 --cd /mnt/e/tmp/EDA -- ./cpp_router/build-contest-linux/contest_router `
  --lef case/open_3d_case001/open_3d_case001.input.lef `
  --def case/open_3d_case001/open_3d_case001.input.def `
  --thermal case/open_3d_case001/open_3d_case001.thermal.input.json `
  --out cpp_router/output/case001-adaptation/case001-linux.guide `
  --report cpp_router/output/case001-adaptation/case001-linux.json `
  --iterations 0 --time-limit 180
```

在 Linux 终端中也可直接运行 `bash cpp_router/build_contest.sh`，再调用 `cpp_router/build-contest-linux/contest_router`。Linux 脚本静态链接 libstdc++/libgcc，系统 glibc/libm 仍动态链接；Windows 比赛程序静态链接 MinGW 运行库。运行求解器不需要 Python、OpenROAD 或第三方 C++ 库。

## 两种报告及评分命令

- `case001-final-initial.json`：**布线器自己的报告**，记录完成网络、内部线长/拥塞目标、热图映射、耗时、内存和本地 guide 检查。`official_score` 始终为 `null`。
- `score-final-initial/evaluation_report.json`：**比赛评分器的报告**，包含合法性和各项分数。提交、调参比较应看它。

本机评分器和 FLUTE 数据表位于 `/home/termer22/1_workspace/8_EDA`，必须从该目录启动，否则可能找不到 `POWV9.dat`：

```powershell
wsl -d ubun2 --cd /home/termer22/1_workspace/8_EDA -- ./score `
  /mnt/e/tmp/EDA/cpp_router/output/case001-adaptation/case001-final-initial.guide `
  --lefs /mnt/e/tmp/EDA/case/open_3d_case001/open_3d_case001.input.lef `
  --input-def /mnt/e/tmp/EDA/case/open_3d_case001/open_3d_case001.input.def `
  --thermal-json /mnt/e/tmp/EDA/case/open_3d_case001/open_3d_case001.thermal.input.json `
  --output-dir /mnt/e/tmp/EDA/cpp_router/output/case001-adaptation/score-final-initial
```

评分器会在用例目录生成/更新 `net_to_die.json`；求解器不会修改输入 LEF、DEF 或热图。

2026-10-14 的 case001 实测值（1.2 默认参数复测 guide 与此版本一致）：

| 项目 | 结果 |
|---|---:|
| 评分器合法性 | PASS |
| 完成网络 / TSV | 3152 / 657 |
| 线长分 | 76.514771 |
| 拥塞分 | 65.144161 |
| 热/TSV 分 | 51.559862 |
| 按 45% / 30% / 25% 汇总 | 66.864861 |
| 评分器总溢出 / 溢出网格层条目 | 12863 / 6896 |
| Windows 初布耗时 / 峰值 RSS | 7.421 秒 / 92.19 MiB |
| Linux 初布耗时 / 峰值 RSS | 5.888 秒 / 88.97 MiB |

加权汇总是按评分报告中的权重计算的派生值，评分器 JSON 本身只有三项分数。耗时含输入处理和输出检查，机器负载会影响结果。Windows 初布、Linux 初布、Windows 两轮重布最终 guide 的 SHA256 均为：

```text
7aa4cec69becf3ea9ef955e659fb39e8d47ec80c903b7cdb12292e109ca622a2
```

**内部 `overflow=0` 仍不能表示拥塞满分。** 当前资源模型统计每条网络在每层每格的占用；尚未加入评分器的固定 pin/OBS 需求和 TSV 两端额外需求。评分器本次算出 12863 的总溢出。内部线长目标也不同于评分器的 guide 矩形 MST 近似。报告已明确标注这些差异；拥塞模型对齐仍是后续优化重点。

## 1.2 的引脚接入与输入兼容

case003 的宏边缘引脚可能处于“真实引脚可接入，但 GCell 中心被 OBS 阻挡”的位置。新版先保持原有接入方式，再尝试 pin 所覆盖网格中的接入点及最多四格的局部逃逸；按实际直角线段检查障碍。接入路径单独记录在 `Terminal.accessPath`，输出 guide 和资源提交/撤销都会包含它，不会全局解除网格障碍。

case005 的内存宏把 OBS 画到了自身 pin 金属上。网格保留 OBS 所属实例，在 pin 实际覆盖的层与坐标允许从自身金属向外换层；中间层必须没有障碍或同样属于该 pin 的金属，其他实例和 DEF 障碍仍有效。

LEF 中倒置的两个矩形角点会在内存中排序，零面积矩形仍报错。原始输入文件不改写；报告记录 `input_compatibility.normalized_lef_rectangles` 及兼容处理警告。`pin_access.escape_terminals` 记录使用局部接入路径的终端数量。

`--astar-weight 1.5` 可以用于大用例寻找完整方案。带权模式通过 `contest_tree_distance.hpp` 的空间索引计算到真实布线树节点的最近 XY/VIA 距离下界，避免把外接矩形内部的大片空白当成目标。die、TSV、障碍和 guide 检查规则仍然生效。默认权重为 1，保留原有搜索；case001 的默认输出与 1.1 版一致。

## 1.1 的主要修改

| 代码 | 用初学者能理解的话说明 |
|---|---|
| `include/router/contest_topology.hpp` | 先移除跨芯片 VIA，将剩下相通的金属层分组，确定哪些层属于同一片芯片 |
| `src/contest_router.cpp` | 跨芯片网络先挑一个共同 TSV 位置，再分别连接两片芯片上的引脚；两侧界面层避开其他重叠位置 |
| `src/net_router.cpp` | 引脚接入与换层也遵守所属芯片的限制 |
| `src/contest_guide.cpp` | 检查实际写出的 guide，包括引脚覆盖、连通、芯片层归属和唯一 TSV 候选 |
| `src/contest_resources.cpp` | 读取稀疏热图、转换原点、计算同格平均温度、补齐空格；跳过无关 JSON 数据的存储 |
| `src/gcell_grid.cpp` | 将比赛网格上限变成可配置预算；障碍扫描只查看附近格子 |
| `src/contest_main.cpp` | 提供参数、输出报告，输入准备耗时从剩余布线预算扣除 |

TSV 可以理解为上下两片芯片之间的连接点。旧程序会让同一网络的不同分支在多个位置跨片，case001 曾被评分器判 0 分。新版让分支共用一个 TSV，并在 guide 检查中防止这种情况再次漏过。本地采用“恰好一个重叠候选”的较严格策略，未实现赛事允许的特定双候选消歧例外。

网络仍按 HPWL/引脚数或 DEF 顺序初布，各片内部用 A* 把引脚接到已有树。候选 TSV 按曼哈顿距离及资源/热代价排序，最多尝试 32 个；距离前缀和减少高扇出网络的重复计算。拆线重布可以暂时变差，但最终按“失败数、内部溢出、跨片数、静态代价”保留最佳结果。它是启发式算法，不保证最优，也没有直接最优化评分器的完整公式。

## 热图输入

接受如下结构（这里只展示格式）：

```json
{"meta":{"coordinateUnit":"micron","temperatureUnit":"degC","ambientTemp":25},
 "nodes":[[0,0,0,30],[10,10,-0.001,32]]}
```

`nodes` 每条为 `[x,y,z,温度]`，支持稀疏采样和同格多条采样。`x,y` 单位支持 `micron` 和 `dbu`；缺单位时按 micron 解释并告警。`z` 区分热图中的 die；投影到 XY 后，将同格的**全部样本**一起求均值，再逐行向右填充、逐列向下填充。行首/顶部空洞使用随后第一个有效值。不是先对每片芯片求均值再强行等权。

坐标模式由 `--thermal-coordinates` 控制：

- `absolute`：与 DEF 使用同一个原点。
- `die-local`：转换 DBU 后，加上 DIEAREA 左下角坐标。
- `auto`：当 DIEAREA 原点非零、热图从约 0 开始且覆盖范围与 die 宽高吻合时采用 die-local；否则使用 absolute。特殊输入应显式指定模式。

正确转换后，允许边界外默认 100 DBU 以内的采样裁剪到边界，并在报告统计；超出容差报错。浮点表示造成的极小边界误差不算物理裁剪。case001 的 120 条采样实际超过右边界 60 DBU。

新 C++ 加载器与 case001 评分器输出的 **64005 个温度格**逐格比较，在 `1e-7` 阈值下 0 个不一致，最大绝对误差约 `3.55e-15`。这项验证覆盖 case001；case003 原点转换及首部缺样本规则没有与该用例的评分器逐格核对。读取时仍拒绝空热图、非有限数值、无效单位和不合法 JSON。无关 `cells` 等字段会校验语法但不保留对象树；文件原文仍整体读入内存。

可用编译出的诊断工具复现温度对照（该工具不布线）：

```powershell
.\cpp_router\build-contest\contest_thermal_check.exe `
  .\case\open_3d_case001\open_3d_case001.input.lef `
  .\case\open_3d_case001\open_3d_case001.input.def `
  .\case\open_3d_case001\open_3d_case001.thermal.input.json `
  .\cpp_router\output\case001-adaptation\score-final-initial\heat_gcell.csv
```

## 参数与退出码

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `--iterations` | 8 | 初布后最多重布几轮，0 表示仅初布 |
| `--time-limit` | 300 | 软时间预算，秒；扣除输入准备时间，输出/回读检查可能额外耗时 |
| `--max-grid-nodes` | 50000000 | 比赛程序网格节点上限；还须满足 int 节点 ID 可表示范围 |
| `--grid-memory-mb` | 4096 | 固定数组的估算预算，单位 MiB；不是进程峰值内存硬限制 |
| `--thermal-coordinates` | auto | auto / absolute / die-local |
| `--thermal-boundary-tolerance-dbu` | 100 | 正确坐标系下的边界裁剪容差，可设为 0 |
| `--order` | short | short / def，初布网络顺序 |
| `--astar-weight` | 1 | `[1,3]`；1 为普通 A*，大于 1 为加权搜索，可加速但不保证单次最短路径 |
| `--via-cost` | 1 | 普通 VIA 等效微米代价，大于 0 |
| `--cross-via-cost` | 8 | 跨片代价，不小于普通 VIA |
| `--cross-via` | 自动识别 | 可重复指定实际被选中的 LEF VIA 名 |
| `--thermal-weight` | 12 | 冷区 TSV 代价；无热图时禁用 |
| `--present-weight` | 2 | 当前拥塞及 TSV 聚集代价 |
| `--history-weight` | 1 | 历史拥塞代价 |
| `--capacity-scale` | 1 | 容量倍率 `(0,1]`，用于实验；不改输入 |
| `--fallback-capacity` | 8 | 没有适用 TRACKS/pitch 时的回退容量 |
| `--gcell-step` | 不猜测 | 缺少 GCELLGRID 的方向需显式给出步长，DBU |
| `--report` | `<out>.json` | 本地报告路径 |

固定数组估算为 `节点数×36.125 + XY格数×44` 字节，还需为输入几何、队列、路径及最佳方案副本留内存。教学程序默认节点上限仍为 200 万。`--lef` 可以重复，用于合并技术 LEF 和单元 LEF。跨片 VIA 默认按选中 VIA 名中的 `tsv/hb/hybrid` 识别，名称不适用时显式指定 `--cross-via`。

`--check 文件.guide` 可以独立检查已有结果，须同时提供 `--lef` 和 `--def`，不能再传 `--out`。默认生成 `<guide>.validation.json`。检查的是格式、网格对齐、范围、区域连通、真实引脚覆盖、die 层和唯一 TSV，不是详细布线 DRC。

退出码 `0`：所有网络成功且本地检查通过；`2`：布线不完整或 guide 无效；`1`：参数/输入/输出错误。超时保留最佳结果；若最佳结果已经完整且通过检查，仍可退出 0。部分结果不可提交。

## 测试范围与其他用例

本轮 Windows **7/7 CTest 通过**，其中包含 28 次 CLI 集成调用；Linux 构建成功并通过 23 次 CLI 集成调用。case003/case005 guide 均通过 Linux 独立检查；没有在 Linux 重跑这两个大用例的完整布线。旧 sample001 位于 `case/case1/cases/sample001`，测试同时兼容早期目录；可用 CMake 参数 `-DROUTER_SAMPLE_DIR=路径` 覆盖。

新增回归覆盖唯一/多候选 TSV、同 die 约束、备用引脚形状顺序、稀疏热图、重复样本均值、坐标平移、边界小误差、37,122,680 节点网格及内存预算、局部 OBS 扫描与全格几何对照。旧 case001 guide 被新版检查器拒绝，报告含原先评分失败的 `net1040`。

| 用例 | 本轮实测状态 |
|---|---|
| open_3d_case001 | 全部布通，Windows/Linux guide 一致，评分器 PASS |
| open_3d_case003 | 151611 / 151611 全部布通，比赛评分器 PASS；最终复测 324.589 秒、峰值 RSS 873.69 MiB，guide 与已评分文件一致 |
| open_3d_case005 | 179700 / 179700 全部布通，Windows/Linux 本地检查 PASS，比赛评分 PASS；本次 Windows 重新布线权重 1.5，44.124 秒、峰值 RSS 2802.11 MiB；加权分 44.701731 |

case003 报告放在 `output/case35-legality/`。线长分为 77.851529，拥塞分为 15.109334，热分为 61.241981，按 45% / 30% / 25% 计算的派生汇总为 54.876483。合法性通过，拥塞评分仍待优化。

case005 本次结果放在 `output/case005-scoring-20261015/`，说明见该目录的 `results.md`，正式报告为 `score/evaluation_report.json`。线长分 **85.555868**、拥塞分 **4.931376**、热分 **18.888711**，按同样权重计算为 **44.701731**。正式总溢出为 2822569，最大溢出为 11；内部溢出为 0 并未对应高拥塞分。评分耗时 21 分 23.65 秒（含包装脚本），峰值 RSS 约 20.98 GiB，拥塞和热评分是后续优化重点。

case005 的 `ADDFHX2_bottom/CI`（metal1）及 `ADDFHX2_top/CI`（metal11）均含 `RECT 2.59 0.40 3.67 0.36 ;`。1.2 保留两个角点，内部解释为 y 范围 `[0.36,0.40]`，记录两次归一化；原始 LEF 保持不变，评分继续使用原始 LEF。

case003 结果说明见 `output/case35-legality/case003-results.md`；比赛报告为该目录下 `score-case003/evaluation_report.json`。早先 case001 详细记录仍保留在 `output/case001-adaptation/`，其中 `case003-smoke.partial.guide` 只是旧超时结果。

`output/case35-legality/run-and-score.ps1 -CaseId 003` 可重新布线并评分，加 `-ScoreOnly` 只评分已有 guide。case005 的评分包装脚本会临时增加 32 GiB swap，评分结束时清理，未修改比赛评分器。本次在约 23.46 GiB RAM、8 GiB 常规 swap 的 WSL 环境中完成正式评分，退出码为 0，临时交换文件已清理。新输出目录的复现命令见 `output/case005-scoring-20261015/results.md`。

现有 `output/contest`、`output/contest-linux` 对比报告及旧提交压缩包属于较早版本，未作为本轮证据、未重新生成。`benchmark_contest.py`、`package_contest.py` 及 OpenROAD 样例脚本已兼容移动后的 sample 目录；重新发布前应先重跑对应实验和验证。

后续重点是对齐评分器的基础占用、TSV 需求和拥塞罚项，再优化 TSV 位置和布线路径。当前最多处理每条网络跨两片 die；复杂 pin 候选、POLYGON/PATH、生成式 VIA、详细线宽/间距、RC/时序等仍未完整支持。对大用例的完整运行时间和最终分数尚无保证。
