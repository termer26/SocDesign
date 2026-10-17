# C++ GCELL / A* 教学布线器

比赛命令行求解器请看 [CONTEST.md](CONTEST.md)：新增 `contest_router`，支持 `.guide`、TRACKS 容量、热图和拆线重布；本页仍说明原有 `learning_router`。

这个程序完成一条完整的学习流程：读取 DEF 和 LEF，计算 instance 的真实 pin 坐标，建立 GCELL 网格，用 A* 连接线网，最后写回带 `ROUTED` 路径的 DEF。

布线器本身全部使用 C++17 和标准库，无第三方 C++ 依赖。各项功能分别放在小文件里，并配有中文注释。原项目的 Python、JavaScript 演示不受影响。

这是**全局布线的教学实现**。输出中的导线位于 GCELL 中心，接入短线连接到真实 pin，换层使用 LEF 中已有的 VIA。它还没有做详细轨道分配、线宽间距检查、不同 net 的短路检查或合法 TSV 站点约束。不同 net 的示意导线可能重叠，不能把输出当作可制造、DRC 合格的详细布线结果。

## 先运行项目样例

在项目根目录 `E:\tmp\EDA` 打开 PowerShell。当前机器已经有 MinGW g++ 和 CMake。

```powershell
cmake -S .\cpp_router -B .\cpp_router\build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build .\cpp_router\build --parallel 4

New-Item -ItemType Directory -Force .\cpp_router\output | Out-Null

.\cpp_router\build\learning_router.exe `
  --lef .\case\case1\cases\sample001\sample001.input.lef `
  --def .\case\case1\cases\sample001\sample001.input.def `
  --out .\cpp_router\output\sample001.routed.def `
  --pins .\cpp_router\output\sample001.pins.csv
```

输出：

```text
Design: die_bottom_min, DBU/micron: 2000
Instances: 218, instance pins: 812, pin rectangles: 1212, IO pins: 12
GCELL: 42 x 42 x 20 routing layers
Nets: routed=203, skipped=0, failed=0, expanded=6411, overflow edges=0
```

`sample001.routed.def` 是保留原布局与连接关系、增加布线路径的 DEF。`sample001.pins.csv` 包含所有 instance pin 和顶层 pin 的**每个矩形**，包括 POWER/GROUND pin。一个 pin 可以有多个矩形，因此 CSV 的行数比 pin 数多。本例有 1212 个 instance pin 矩形和 12 个顶层 pin 矩形。

CSV 的 `x1_dbu`、`y1_dbu`、`x2_dbu`、`y2_dbu` 是绝对坐标。中心坐标供寻路选接入点使用；pin 的实际几何仍然是矩形，不是一个没有面积的点。换算为微米时除以 DEF 中的 `UNITS DISTANCE MICRONS`，这里是 2000。

不传 `--pins` 时，程序自动在输出 DEF 旁写一个同名 `.pins.csv`。输出目录需提前创建。重复运行会更新指定的输出文件，程序禁止用输入 DEF/LEF 的路径作输出。

Linux/WSL 上也可以编译。安装了 CMake 时使用独立构建目录：

```bash
cmake -S cpp_router -B cpp_router/build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build cpp_router/build-linux -j4
```

也可以直接用 g++ 编译，无需 CMake：

```bash
mkdir -p cpp_router/build-linux
g++ -std=c++17 -O2 -Wall -Wextra -Icpp_router/include \
  cpp_router/src/{tokenizer,lef_parser,def_parser,pin_resolver,gcell_grid,astar,net_router,def_writer,main}.cpp \
  -o cpp_router/build-linux/learning_router
```

## 功能如何分开

推荐按下表的顺序阅读。每个模块在 `include/router` 中有对应的 `.hpp`，声明它提供的接口；实现位于 `src` 中。

| 文件 | 只需要关注的问题 | 主要入口 |
| --- | --- | --- |
| `types.hpp` | 程序需要存储哪些数据？ | `Instance`、`LefPin`、`Net`、`Cell` |
| `main.cpp` | 各模块如何串起来？ | `main` |
| `tokenizer.cpp` | 如何把文本拆成单词、括号和分号？ | `tokenize`、`Cursor` |
| `lef_parser.cpp` | 单元多大，pin 在哪里，哪些层能布线？ | `parseLef` |
| `def_parser.cpp` | 单元放在哪里，哪些 pin 要连接？ | `parseDef` |
| `pin_resolver.cpp` | 如何把局部 pin 几何变成绝对坐标？ | `resolvePins` |
| `gcell_grid.cpp` | 物理坐标如何映射到网格和金属层？ | `GCellGrid` |
| `astar.cpp` | 起点如何找到到达终点的路径？ | `findPath` |
| `net_router.cpp` | 多 pin、多个 net 如何分别布线？ | `routeNet`、`routeNets` |
| `def_writer.cpp` | 如何把网格路径写成 DEF 导线？ | `writeRoutedDef` |

整体数据流：

```text
LEF ── parseLef ── Library ─────┐
                              ├── resolvePins ── PinDatabase ── pin CSV
DEF ── parseDef ── Design ─────┘                        │
                    │                                 │
                    └── GCellGrid ── routeNets / A* ───┘
                                            │
                                        RoutedNet[]
                                            │
                                      writeRoutedDef
                                            │
                                        输出 DEF
```

头文件使用 `#pragma once` 防止重复包含。`namespace router` 将这些类型与函数放在同一个命名空间。暂时不必钻研 CMake，可以先从 `main.cpp` 看整个流程，再进入每个函数。

## 为什么还需要 LEF

DEF 中的一个 component 例如：

```def
- u_buf BUF_X1_bottom + PLACED ( 53200 204400 ) FS ;
```

它告诉我们 instance 名称、master 名称、放置位置和朝向。它并没有给出 `A`、`Z` 等 instance pin 的内部形状。

LEF 中的 `MACRO BUF_X1_bottom` 包含 `SIZE`、`ORIGIN`、`PIN`、`PORT`、`LAYER`、`RECT`。所以程序先通过 master 找到 LEF macro，再通过 pin 名找到内部矩形。

坐标计算顺序是：

1. 把 LEF 的微米坐标按 DEF 的 DBU 单位转换为整数，并处理 LEF `ORIGIN`。
2. 对矩形的四个角应用 DEF 朝向。
3. 按旋转后的单元外框补齐位置，使其左下角位于 DEF 的 component 放置点。
4. 对转换后的四个角取最小/最大值，得到绝对矩形。

八种朝向是 `N/S/E/W/FN/FS/FE/FW`。旋转/镜像的定义集中在 `rotatePoint`，单元外框处理集中在 `placeMacroPoint`，不散落在 parser 或 A* 中。

顶层 pin 出现在 DEF 的 `PINS` 中，以 `( PIN pinName )` 参与 net 连接。它的矩形相对于 PORT 放置原点，只需要旋转/镜像再平移，不做 macro 外框补齐。程序同时支持显式 `+ PORT` 和直接挂在 pin 下的矩形。

## GCELL 是什么

`GCELLGRID` 给出全局布线网格线，不是标准单元的 `ROW`，也不是详细布线的 `TRACKS`。

```def
GCELLGRID X 0 DO 42 STEP 5700 ;
```

这表示 42 条 X 方向网格线：`0, 5700, ... , 233700`。相邻两条线之间才是格子。程序将同轴多个 pattern 合并、去重、按 DIEAREA 裁剪，并补上 DIEAREA 的两端边界。因此，**不能直接把 DO 当作格子数**。

项目样例的右边界是 240920，超过最后一条网格线，补上边界后得到 42 个 X 格子，最后一个格子较宽。Y 方向也有相同处理。

`locate` 使用二分查找定位格子，支持负坐标、非零原点和非均匀网格。内部网格线上的点归右侧/上侧，芯片最外边界上的点归最后一个格子。

一个搜索节点是 `Cell{x, y, layer}`。A* 走的是格子中心之间的边，而不是对每个 DBU 都建一个节点。`id` 把三维坐标转换为数组下标，`cell` 做逆转换：

```cpp
id = (layer * height + y) * width + x;
```

金属层只从 LEF 的 `TYPE ROUTING` 中选取。层与层之间是否相连由 LEF 中已有的 VIA 决定，不靠名字猜测。每对金属层优先选择第一个 DEFAULT VIA；没有 DEFAULT 时选择第一个已有 VIA。样例的 metal10 / metal11 使用 `hb_layer_0`。程序不把 instance 名中的 `_top`、`_bottom` 当作层号，也不会凭空创建跨层连接。

## A* 如何搜索

先单独阅读 `astar.cpp`，它不负责解析 DEF，也不负责写文件。

```text
g = 从起点走到当前节点的实际代价
h = 从当前节点到目标的剩余代价下界
f = g + h
```

`priority_queue` 每次取 `f` 最小的节点。对每个可走的邻居计算新的 `g`；如果比旧值更小，就更新距离、记录 `parent`，并加入队列。命中目标后沿 `parent` 反向回溯，再反转得到起点到终点的路径。

同层水平/垂直移动使用真实 GCELL 中心的距离；沿金属层非首选方向移动时乘以 2。过孔有单独的正代价。之前 net 的资源占用会增加后续搜索的代价。

单终点的 XY 启发是曼哈顿距离；多终点使用目标集合外框的曼哈顿距离下界。再加上 LEF 过孔图中的最短换层代价下界。启发函数不计算拥塞和障碍惩罚，因此不高估实际剩余代价。A* 对**当前固定代价图**求最短路径；这不等于全部线网的总线长最优，也不包含 pin 接入短线的优化。

## 两个 pin 与多个 pin

两个 pin 直接调用 A*。多个 pin 在 `routeNet` 中按 DEF 顺序处理：

```text
树 = 第一个 pin 所在节点
依次处理剩余 pin：
    从这个 pin 搜索到已有树的任意节点
    把找到的路径合并进树
全部成功后才提交资源占用
```

`findPath` 接受一组目标，所以树中任意节点都可以作为接入点。同一个 GCELL 中的 pin 仍然各有接入短线，不会因为网格节点重合而遗漏实际连接。这是逐步建树的贪心算法，不保证全局最优 Steiner 树。

一个矩形 pin 通常从矩形中心接入。程序先尝试该 pin 的各个矩形；如果它们所在格子的中心都被遮挡，会在真实 pin 坐标处沿 VIA 图寻找可用金属层，再连接到该层的 GCELL 中心。这部分逻辑单独放在 `chooseTerminal`。

## 障碍与拥塞的范围

宏单元和非 CORE 单元的 LEF `OBS RECT`，以及 DEF `BLOCKAGES LAYER ... RECT` 作为硬障碍。格子中心落在障碍内则禁止该节点，中心之间的格边横穿障碍也会禁止。

CORE 标准单元的微小 OBS 不作为整个 GCELL 的硬禁区，它们应在详细布线/容量建模中处理。放置用的 `BLOCKAGES PLACEMENT` 不等于金属障碍，也不会当作布线禁区。

这里检查的是网格中心线，没有按线宽/间距扩张障碍。pin 接入短线没有做完整障碍绕行或详细过孔包围检查。

拥塞模型使用每条格边的占用数，每个 net 对共享格边只计一次。`capacity` 是人为指定的教学容量，不是从 TRACKS 精确推导的工艺资源；超容量增加代价，不会强制禁止通行。程序没有拆线重布或顺序优化。`overflow edges=0` 仅表示这个抽象容量模型未超限，不代表 DRC 合格。

## 如何输出 DEF

`def_writer.cpp` 把 pin 接入短线与 A* 网格分支分别转换成同层曼哈顿折线。共线中间点会被压缩，换层则在路径上追加 LEF VIA 名。例如：

```def
- net1 ( u1 Z ) ( u2 A )
  + USE SIGNAL
  + ROUTED metal1 ( 100 200 ) ( 300 200 ) via1_4
    NEW metal2 ( 300 200 ) ( 300 600 ) ;
```

parser 记录了每条 net 和旧布线属性在原 DEF 文本中的位置。writer 只替换成功 net 的 `ROUTED/FIXED/COVER/SHIELD/NOSHIELD` 布线属性，并保留其连接列表与其它属性。其它 DEF 区段保持原文，包括 ROW、TRACKS、COMPONENTS、PINS、SPECIALNETS 等。

POWER/GROUND net 和不足两个连接的 net 跳过；失败 net 保留原文和旧布线，不输出部分分支，也不计入新资源占用。已有 SPECIALNETS 和跳过/失败 net 的旧导线不参与拥塞模型。

返回码 `0` 表示所有应布线的 net 完成，`2` 表示有 net 未连通，`1` 表示输入或输出错误。失败原因会逐 net 输出到标准错误。

## 小例子与参数

先用只有 8 个 instance、3 个信号 net 的 `tests/fixtures/tiny.def` 理解代码。它配套的 LEF 有非零 ORIGIN、八种朝向，以及两层金属。

```powershell
.\cpp_router\build\learning_router.exe `
  --lef .\cpp_router\tests\fixtures\tiny.lef `
  --def .\cpp_router\tests\fixtures\tiny.def `
  --out .\cpp_router\output\tiny.routed.def
```

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--lef` | 必填 | 包含工艺层、VIA、macro 的配套 LEF |
| `--def` | 必填 | 已放置的输入 DEF |
| `--out` | 必填 | 输出 DEF |
| `--pins` | 根据输出名生成 | pin 矩形 CSV |
| `--gcell-step` | 不自动猜测 | 缺少某个方向 GCELLGRID 时的补充间距，单位 DBU |
| `--via-cost` | 5 | 单次换层的等效微米代价，必须为正 |
| `--capacity` | 8 | 每条格边的教学容量，必须为正整数 |
| `--congestion-weight` | 2 | 拥塞代价的权重，必须非负 |

`--gcell-step` 只补缺失方向，不改变已有 GCELLGRID。网格最多支持 200 万节点，过大输入会明确报错。

实现只覆盖矩形 pin/OBS、矩形 DIEAREA、普通 NETS，以及 LEF 中显式定义的 VIA。POLYGON/PATH pin、RECT ITERATE、生成式 VIA、SUBNET/VPIN/MUSTJOIN 不支持，会报错。DEF 的 VIAS 区段保留原文，但寻路不使用其中的过孔。它不是完整的 LEF/DEF 标准解析器。

## 已完成的验证

```powershell
ctest --test-dir .\cpp_router\build --output-on-failure
```

C++ 测试包含 1616 项检查，覆盖八种朝向、LEF ORIGIN、多个 PORT、顶层 pin、负坐标/非均匀 GCELL、末端格子、窄障碍、pin 换层接入、多 pin 连通性、同点连接、无路径、缺失过孔/引脚、旧布线替换和项目样例。100 组随机地图使用独立 Dijkstra 算法核对 A* 的可达性和最短代价。

已在本机 WSL 的 OpenROAD 中完成独立回读：小例子 3 条 net 和项目样例 203 条 net 都被 OpenROAD 正常读取；34 和 1224 个 pin 矩形分别逐一与 OpenDB 坐标比对一致。验证脚本还直接从输出 DEF 的几何线段与过孔检查每条 net 的连通性和真实 pin 接入。结果在 `output/openroad_validation.json`。

可复现该验证，先生成两组输出，再运行：

```powershell
wsl -d ubun2 -- openroad -exit -python /mnt/e/tmp/EDA/cpp_router/tests/validate_openroad.py
```

这一个 Python 文件只是独立的 OpenROAD 验证工具，布线器和常规测试均为 C++。回读和单 net 连通性验证不包含不同 net 之间的短路检查或 DRC。

## 建议的学习练习

1. 在 `tiny.def` 中修改一个 instance 的位置，观察 CSV 与 DEF 路径怎样改变。
2. 把它的 `N` 改成 `FS`，检查 pin 矩形的 Y 坐标与放置点的关系。
3. 在一个 net 中增加第四个 pin，观察它如何接入已有树。
4. 在 DEF 中加入 `BLOCKAGES LAYER M1 RECT ...`，观察 A* 绕行或换层。
5. 调大 `--via-cost`，比较路径是否减少过孔。
6. 调小 `--capacity`，观察拥塞代价如何改变后续 net 的路径。

学懂这些模块后，可以依次扩展轨道容量、详细 pin access、拆线重布和详细布线 DRC。
