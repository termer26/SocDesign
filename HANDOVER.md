# 交接说明：3D IC 全局布线比赛 —— 评分与优化

> 接收方：接手后续工作的 AI 助手
> 撰写时间：本会话结束时
> 所有数字均来自**本机实跑**，不是估算。文中明确区分「已验证」与「推测」。

---

## 0. 一句话现状

参赛者已有一个能跑通的 C++ 17 三维全局布线器（`contest_router`），在官方公开样例 `sample001` 上**合法性和效率都通过**，当前自算总分 **68.33**。
瓶颈**不在拥塞**（那项 96% 的扣分来自用例里宏单元的 OBS，与布线器无关），**而在线长**：走线比 FLUTE 三维 HPWL 基准长 **34.4%**。
**下一步唯一值得做的事：分析并缩短线长。**

---

## 1. 环境

| 项 | 值 |
| --- | --- |
| Windows 工作区 | `E:\tmp\EDA\cpp_router` |
| 项目实际根目录 | `E:\tmp\EDA`（项目 README 里的相对路径都从这里算） |
| WSL 发行版 | `ubun2` = **Ubuntu 22.04.5 LTS**，内核 6.18.33.2-microsoft-standard-WSL2 |
| WSL 用户 / 家目录 | `termer22` / `/home/termer22` |
| WSL 工作目录 | `/home/termer22/1_workspace/8_EDA` |
| 路径对照 | `E:\tmp\EDA\...` ↔ `/mnt/e/tmp/EDA/...` |
| 磁盘 | WSL 家目录可用 936 GB，`/mnt/e` 可用 428 GB —— 都够 |

### 从 Windows 侧驱动 WSL（如果当前在 Windows）

```powershell
wsl -d ubun2 -- bash /mnt/e/tmp/EDA/cpp_router/some_script.sh
```

两个必须知道的坑：

1. **`wsl.exe` 会把 argv 用空格拼接**，所以 `wsl -d ubun2 -- bash -lc 'echo a; echo b'` 里的引号会丢失，命令被截断。**把多行脚本写成 `.sh` 文件再传单个路径**，这是唯一可靠的方式。
2. Windows 侧写的 `.sh` 可能是 CRLF 换行，bash 会报 `$'\r': command not found`。写完后先转 LF。

---

## 2. 关键文件清单

### 2.1 评分器与官方样例（Windows 盘，WSL 里走 `/mnt/e/`）

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `E:\tmp\EDA\cpp_router\score\score` | 21,401,592 B | **官方评分器**。ELF64 x86-64，动态链接 `/lib64/ld-linux-x86-64.so.2`，内部是 **PyInstaller 单文件包，内嵌 CPython 3.10**。只能在 Linux 上跑 |
| `E:\tmp\EDA\case\cases\sample001\sample001.input.lef` | 42,253 | 工艺 + 单元 LEF（20 个 ROUTING 层 metal1–20、20 个 CUT 层、56 个 VIA、8 个 MACRO） |
| `...\sample001.input.def` | 44,873 | 版图 DEF（`UNITS 2000`、`DIEAREA (0 0)(240920 240800)`、`NETS 203`、`GCELLGRID X/Y 0 DO 42 STEP 5700`、40 条 TRACKS、218 个元件） |
| `...\sample001.thermal.input.json` | 240,529 | 热图。`ambientTemp 25.0`、`gridResolution [42,42]`、3528 个节点（两 die 各 1764）、30.94–36.92 °C。**`synthetic: true`、`signoff_usable: false`** |
| `...\sample001.output.guide` | 61,509 | 赛题方给的样例答案。**⚠️ 它通不过当前评分器，见 §5.3** |
| `...\sample001.baseline.guide` | 13,165 | 另一份稀疏对照，本身合法（0 个越界 net） |
| `...\evaluation_report.json` | 2,218 | **⚠️ 这是 mock，不是这个评分器产出的**，见 §5.4 |

### 2.2 参赛者的布线器项目

| 文件 | 说明 |
| --- | --- |
| `E:\tmp\EDA\cpp_router\README.md` | `learning_router`（教学版）说明 |
| `E:\tmp\EDA\cpp_router\CONTEST.md` | **`contest_router`（比赛版）说明，参数表、算法模块、实测指标都在这** |
| `E:\tmp\EDA\cpp_router\src\contest_*.cpp` | 比赛版核心：`contest_resources` / `contest_router` / `contest_guide` / `contest_main` |
| `E:\tmp\EDA\cpp_router\output\contest\optimized.guide` | **当前被评对象**（203 net / 578 矩形） |
| `E:\tmp\EDA\cpp_router\output\contest\{baseline,stress_baseline,stress_optimized}.guide` | 其他对照 guide |
| `E:\tmp\EDA\cpp_router\output\contest-linux\` | 同一批 guide 的 Linux 版输出，与 Windows 版**逐位相同** |
| `E:\tmp\EDA\cpp_router\build-contest\contest_router.exe` | Windows 可执行 |
| `E:\tmp\EDA\cpp_router\build-contest-linux\contest_router` | Linux 可执行 |

`output/` 下共 23 份 `.guide` 可选作被评对象。

### 2.3 WSL 侧

```
/home/termer22/1_workspace/8_EDA/
├── score                          # 评分器副本，与 /mnt/e 那份 md5 相同
├── sample001.input.lef            # md5 与官方逐字节一致
├── sample001.input.def            # md5 与官方逐字节一致
├── sample001.thermal.input.json   # md5 与官方逐字节一致
├── sample001.routed.def           # 参赛者 learning_router 的输出（评分器不读 DEF，无用）
├── README.md                      # 就是评分器说明
├── POST9.dat / POWV9.dat          # ⚠️ FLUTE 基准数据，与评分器无关，可删
├── pin_positions.json             # 评分器写的缓存（放在 --input-def 所在目录）
├── net_to_die.json                # 评分器写的 net→die 归属表（放在 cwd）
└── score-out/                     # 历次评测结果，每个子目录里有 evaluation_report.{json,txt} 和 full.log
    ├── logrun/full.log            # ★ 完整日志，含逐格溢出明细，分析时用这个
    ├── official/                  # 官方 guide 的 FAIL 证据
    ├── own-contest/               # 参赛者 guide 的 PASS 结果
    ├── no11/ no12/ no1112/ noobs/ # 对照实验（删 OBS）
    └── ...
```

### 2.4 本次会话产出的额外文件

| 文件 | 说明 |
| --- | --- |
| `E:\tmp\EDA\cpp_router\output\congestion_explained.html` | **11 步交互式动画**，讲解拥塞扣分归因链。支持 `#s=N` 锚点定位、`←/→` 翻页 |
| `E:\tmp\EDA\cpp_router\.scratch\score_spec.txt` | 评分标准文档（`.docx`）的纯文本版，10 KB，比读 Word 方便 |
| `E:\tmp\EDA\cpp_router\.scratch\cli_walkthrough.txt` | 四个输入文件的规模核对数据 |

评分标准原文（`.docx`）：`E:\tmp\EDA\cpp_router\score\3DIC全局布线评分程序评价标准说明_20291011.docx`

---

## 3. 评分器用法（**已实测，与文档有出入，以本节为准**）

### 3.1 真实 CLI（跑 `./score --help` 得到）

```
usage: score [-h] --lefs LEFS --input-def INPUT_DEF --thermal-json THERMAL_JSON
             [--tsv-cost-ratio TSV_COST_RATIO]
             [--via-cost-ratio VIA_COST_RATIO]
             [--tsv-congestion-ratio TSV_CONGESTION_RATIO]
             --output-dir OUTPUT_DIR
             guide_file
```

- `guide_file` 是位置参数，其余 `--lefs` / `--input-def` / `--thermal-json` / `--output-dir` **全部必需**。
- `--lefs` 是**单值**（若支持多值 argparse 会显示 `LEFS [LEFS ...]`），文档里的复数写法有误导。
- 三个 `*-ratio` 参数在文档里**完全没提**，它们对应文档 §3.1 的 `tsv_ratio` / `via_ratio` 和 §3.2 的 TSV 拥塞。

### 3.2 可直接复制的运行命令

```bash
cd /home/termer22/1_workspace/8_EDA
chmod +x score          # 家目录是 ext4，权限真生效；/mnt/e 上是 777 不需要
P="$PWD"

./score /mnt/e/tmp/EDA/cpp_router/output/contest/optimized.guide \
  --lefs         "$P/sample001.input.lef" \
  --input-def    "$P/sample001.input.def" \
  --thermal-json "$P/sample001.thermal.input.json" \
  --output-dir   "$P/score-out/run1"
```

### 3.3 ⚠️ `--input-def` 必须用绝对路径（最容易踩的坑）

评分器把引脚几何缓存 `pin_positions.json` 写到 `--input-def` 的目录。相对路径的目录部分是空串，拼出来变成 **`/pin_positions.json`**（文件系统根目录），非 root 写不进去，评测直接中断：

```
【输入文件】 违反规则：输入文件必须存在且可读
问题：[Errno 13] Permission denied: '/pin_positions.json'
评测中断，无法给出得分
```

对照实验（**已验证**）：

| 相对路径的参数 | 结果 |
| --- | --- |
| 只有 `--input-def` 相对 | ❌ 中断 |
| 只有 `--lefs` 相对 | ✅ 正常 |
| 只有 `--thermal-json` 相对 | ✅ 正常 |
| 全部绝对 | ✅ 正常 |

官方 Docker 流程能跑是因为容器里是 root，能往 `/` 写 —— 那条官方命令本身也带着这个雷。

### 3.4 退出码不等于通过

- `exit=0` = 评测**跑完了**（`status` 可能是 `PASS` 也可能是 `FAIL`）
- `exit=1` = 评测**中断**（输入文件问题）

**判通过只看 `evaluation_report.json` 里的 `status` 字段。** 终端在通过时不打印 "PASS"，只有失败时才跳红色大字。

### 3.5 产出与副作用

`--output-dir` 下**只有两个文件**（文档 §5.3 承诺的 5 个 CSV 一个都没生成）：

- `evaluation_report.json`
- `evaluation_report.txt`

另外评分器会往别处写两个文件：

- `pin_positions.json` → 放在 `--input-def` 所在目录
- `net_to_die.json` → 放在**当前工作目录**（不是 `--output-dir`）

**报告里没有 `total_score` 字段**，总分要自己按 45/30/25 加权算。

### 3.6 库依赖

实测 `--help` 与完整评测都能跑通，**不需要额外配置**。系统里只有 `libpython3.10.so.1` 和 `.so.1.0`（没有裸 `.so`），PyInstaller 自己解决了。官方 README 里的 `LD_LIBRARY_PATH` / `PYTHONPATH` 在这个环境下**不需要**（PyInstaller 自带解释器，不读外部 `PYTHONPATH`）。

---

## 4. 评分规则

```
Score = S1 × (S2 + S3) × S4
```

| 项 | 满分/权重 | 判定 | 否决 |
| --- | --- | --- | --- |
| S1 合法性 | 1 | ① 全部 DEF net 连通、每个引脚被同层矩形覆盖、GUIDE 不得出现 DEF 未定义的 net ② 每个矩形必须与 DIEAREA 有重叠 ③ 层必须是 LEF 声明的 ROUTING 层，且符合跨 Die/TSV 规则 ④ GUIDE 格式可解析 | 是 |
| S2 优化质量（公开用例） | 30 | 线长 45% + 拥塞 30% + 热 25%，由主办方按名次折算 | 否 |
| S3 优化质量（隐藏用例） | 70 | 同上（拿不到） | 否 |
| S4 运行效率 | 1 | 单用例 ≤ 12 小时、峰值内存 ≤ 128 GB | 是 |

**三项质量分各自 0–100**，评分器只给这三项，`: 名次折算由赛题组织方完成`。

### 4.1 线长（权重 45%）

```
L = total_cost / HPWL_base_cost
q_wl = 100 × min(L, 1 / L)          ← 偏离 1.0 就掉分，偏大偏小都掉
```

- `total_cost = 平面部分 + via_number × via_cost + cross_cost(跨DIE net)`
- 平面部分 = `Σ_层 (该层被覆盖的 GCell 数 × 该层 GCell 边长)`
- `HPWL_base_cost` = FLUTE 直角斯坦纳树长度 + FLUTE 层间连接数 × via 代价 + 跨 die 成本

### 4.2 拥塞（权重 30%）

```
Overflow_i = max(0, demand_i − capacity_i)          逐 (GCell, 层)
sum_over_r = Σ (0.1 × Overflow_i / capacity_i)²
q_cong = 100 / √(1 + sum_over_r)
```

demand 三来源：**障碍（引脚/OBS）按实际占用 track 数 + GUIDE 每 net 每格每层记 1 份 + 每个 TSV 在 top/bottom 层各记 1 份**。

### 4.3 热（权重 25%）

```
q_thermal = 100 × (0.5 + 0.5 × tanh(G_case × r))
```

`r` = TSV 分布与温升分布（`max(T − ambient, 0)`）的皮尔逊相关系数。
`G_case` 实测是 **20.0，来源 `default`** —— 评分器去找 `thermal_gain.json` 没找到就退化。官方评测若提供该表，同一份 guide 的热分会不同。

### 4.4 分数自算方法

```python
total = 0.45*wirelength_score + 0.30*congestion_score + 0.25*thermal_score
```

---

## 5. 已完成的实测结论（**不要重复验证这些**）

### 5.1 GCell 与容量模型（**已验证，与评分器日志 51/51 逐格吻合**）

- 网格 **42 列 × 42 行**。`GCELLGRID 0 DO 42 STEP 5700` 给出 42 条线（0 … 233700），DIEAREA 右边界 240920 超出末线，评分器**补一个边缘窄格** → 正好 42。热图声明的 42×42 与此对应。
- **容量 = 该层首选方向的 track 位置落在该 GCell 跨度内的个数**：

| 层 | 首选方向 | TRACKS 步距 | 容量 |
| --- | --- | --- | --- |
| metal1 / 3 / 18 / 20 | H / H / V / V | 280 DBU | **20 – 25** |
| metal2 / 19 | V / H | 380 | 15 – 19 |
| metal4 / 5 / 6 / 15 / 16 / 17 | — | 560 | 10 – 13 |
| metal7 / 8 / 13 / 14 | — | 1600 | 3 – 5 |
| **metal9 / 10 / 11 / 12** | — | **3200** | **只有 1 或 2** |

- metal11 容量只跟**行**有关，容量=1 的行是 `[0,4,8,13,18,22,27,31,36,40]`。
- metal12 容量只跟**列**有关，容量=1 的列是 `[0,4,8,13,18,22,27,31,36,40]`。
- 根因：GCell 宽 5700 DBU，而这四层的 track 步距是 3200 → `5700/3200 = 1.78`，只能放 1–2 条。

### 5.2 当前分数（**已验证**）

对 `output/contest/optimized.guide`：

| 量 | 值 |
| --- | --- |
| `status` | **PASS** |
| `wirelength_score` | **74.4292** |
| `congestion_score` | **73.5334** |
| `thermal_score` | **51.0939** |
| 自算 `total` | **68.3266** |
| `sum_over_r` | 0.8493975623582746 |
| `total_overflow` / `overflow_entries` / `max_overflow` | 450 / 358 / 6 |
| 线长 `total_um` vs `hpwl_reference_dbu` | 3881.7000 µm vs 2889.1175 µm |
| 归一化 `L` | **1.34355906**（偏长 34.4%） |
| TSV 数 | 3 |
| 警告 | `GCELLGRID X/Y ended before DIEAREA`（正常，补边缘格）、`thermal score uses a deterministic synthetic placement-density map` |

### 5.3 ⚠️ 官方样例 guide 本身违法

`sample001.output.guide` 评测结果是 **`status: FAIL`，0 分**：

```
【合法性检查】违反规则：满足 LEF 中定义的设计规则
问题：net 'TOP__l0_b10' 的引脚都在顶层 Die，但 GUIDE 中使用了底层 Die 的金属层
建议：仅位于顶层 Die 的 net 不应使用底层 Die 的布线层。
```

复核（**已验证**）：

| guide | TOP__ net 误用 metal1–10 | BOTTOM__ net 误用 metal11–20 |
| --- | --- | --- |
| 官方 `sample001.output.guide` | **31 个** ❌ | 0 |
| 官方 `sample001.baseline.guide` | 0 ✅ | 0 |
| 参赛者 `contest/optimized.guide` | 0 ✅ | 0 |

**所以别拿 `sample001.output.guide` 当标准答案基线**，对照要用 `baseline.guide`。

### 5.4 ⚠️ `case/` 里那份 `evaluation_report.json` 是 mock

它自己写着 `"drc": "not available"`、`"thermal": "synthetic"`，`inputs` 指向 `/data/code/eda-contest/...` 和 `sample001.thermal.output.json`（本机不存在）。`CONTEST.md` 第 5 行也确认了这点。**不要拿它的字段或分数当基准**（它写 `total_score 81.24`、`tsv_cost_dbu_each 2000.0`，与真实评分器完全不同）。

### 5.5 拥塞归因：96% 来自宏的 OBS（**已验证，受控实验**）

`DEMO_MACRO_top`（15.2 × 14.0 µm，3 个实例）在 LEF 里的 OBS 只有两行：

```
OBS
  LAYER metal11 ;  RECT 0.0 0.0 15.2 14.0 ;
  LAYER metal12 ;  RECT 0.0 0.0 15.2 14.0 ;
END
```

三个实例落点（DEF 朝向只有 `N` 127 个、`FS` 91 个）：

| 实例 | 位置 | 覆盖列 | 覆盖行 |
| --- | --- | --- | --- |
| `u_macro0_top` | (21660, 16800) | 3–9 | 2–7 |
| `u_macro1_top` | (104500, 16800) | 18–23 | 2–7 |
| `u_macro2_top` | (187340, 16800) | 32–38 | 2–7 |

**受控实验**：只把 LEF 里那个 OBS 块删掉（宏还在、版图不变、guide 一个字没改），其余照原样重评：

| 配置 | `sum_over_r` | 溢出格 | 拥塞分 |
| --- | --- | --- | --- |
| 原样 | 0.8494 | 358 | **73.53** |
| 只删 metal11 的 OBS | 0.4994 | 278 | 81.67 |
| 只删 metal12 的 OBS | 0.3844 | 262 | 84.99 |
| 两个都删 | 0.0344 | 182 | **98.32** |

分解出的**整数解与实测完全吻合**：

```
metal11 OBS   20 格 × (0.1×1/1)² = 0.2000
              60 格 × (0.1×1/2)² = 0.1500   → 0.3500
metal12 OBS   30 格 × (0.1×1/1)² = 0.3000
              66 格 × (0.1×1/2)² = 0.1650   → 0.4650
布线器自身                                  → 0.0344
──────────────────────────────────────────────────
sum_over_r                                    0.8494
拥塞分 = 100/√(1.8494)                         73.53
```

辅助证据：

- 参赛者的 guide 在 metal9–12 上**只用了 13 个格**（对比 metal19 用了 142 个矩形、metal1 用了 122 个）。评测器日志逐条打印的 51 个溢出格里，**50 个根本没有它的走线**。
- 引脚贡献为 0：`BUF_X1_top` 引脚全在 metal19，`BUF_X1_bottom` 全在 metal1，`DEMO_MACRO_*` 在 metal13 —— metal11/metal12 上没有引脚。

**结论：拥塞这项已无优化空间。** `0.815 / 0.8494 = 96.0%` 的扣分来自用例里宏单元的 OBS 撞上 3200 步距的稀疏层，是评分模型 + 用例设计的问题，不是布线器能改的。

> ⚠️ 我（上一任助手）中途曾根据日志里 51 个 cap=1 的格估出「60%」，**那个数字是错的**，正确的归因是上面的 96%。日志只打印最严重的一批，OBS 还在 126 个容量 2 的格里各贡献 0.0025，这些格没被日志打印。

---

## 6. 待办：线长（**唯一值得投入的方向**）

### 6.1 差距在哪

```
total_um         = 3881.7000 µm      你的走线总代价
hpwl_reference   = 2889.1175 µm      FLUTE 三维 HPWL 基准
L = 1.34355906                       偏长 34.4%
q_wl = 100 / 1.3436 = 74.43
```

得分是 `100 × min(L, 1/L)`，**L 越接近 1.0 越高**。若能把 L 压到 1.05，`q_wl ≈ 95.2`，总分 **+9.3**。

### 6.2 建议的第一步：逐 net 归因

评分器的代价模型是逐 net 算的，但报告只给总量。建议自己实现一遍逐 net 拆解：

对每条 net 算
```
你的代价 = Σ_层(覆盖的 GCell 数 × 该层格边长) + via数×via_cost + cross_cost(跨die)
基准代价 = FLUTE_Steiner_长度 + FLUTE_via数×via_cost + cross_cost(跨die)
比值 = 你的代价 / 基准代价
```
按比值排序，找出最拖后腿的几十条。重点看：

- **多端 net 的拓扑**：`contest_router` 用的是「不断选取离已有树最近的剩余终端」的贪心近似树，**没有接 FLUTE**（`CONTEST.md` 第 129 行自认）。多端 net 的数量和 pin 数是关键。
- **pin 接入层选择**：接入短线和换层会引入 via，via 在代价里是 `via_number × via_cost`（`--via-cost` 默认 1 µm）。
- **跨 die 成本**：`tsv_cost_dbu_each` 在这个用例里实测是 **6.0**（不是 mock 报告写的 2000.0），`cross_cost` 只在跨 DIE net 上计一次。

### 6.3 可以调的参数（`CONTEST.md` 有完整表）

| 参数 | 默认 | 影响 |
| --- | --- | --- |
| `--iterations` | 8 | 拆线重布轮数 |
| `--via-cost` | 1 | 普通过孔等效微米代价 |
| `--cross-via-cost` | 8 | 跨 die 代价，必须 ≥ `--via-cost` |
| `--thermal-weight` | 12 | 冷区跨 die 惩罚 |
| `--present-weight` | 2 | 当前拥塞与跨 die 聚集惩罚 |
| `--history-weight` | 1 | 历史溢出惩罚 |
| `--order` | short | `short` 或 `def`，初布网络顺序 |
| `--time-limit` | 300 | 软时间预算（秒） |

### 6.4 不建议投入的方向

- **拥塞**：见 §5.5，96% 是结构性的，剩下 1.68 分要清零 274 条溢出才拿得到。
- **热**：这份热图 `synthetic: true` / `signoff_usable: false`，r 只有 0.001（等于随机）。调 TSV 位置去迎合它在真实评测里没有意义。
- **合法性**：已通过，且参赛者的 router 在 die/层归属这条规则上是对的（官方样例反而错了）。

---

## 7. 复现与工具

### 7.1 评任意一份 guide

```bash
cd /home/termer22/1_workspace/8_EDA
P="$PWD"
for g in /mnt/e/tmp/EDA/cpp_router/output/contest/*.guide; do
  n=$(basename "$g" .guide)
  ./score "$g" --lefs "$P/sample001.input.lef" --input-def "$P/sample001.input.def" \
    --thermal-json "$P/sample001.thermal.input.json" --output-dir "$P/score-out/$n" > "$P/score-out/$n.log" 2>&1
  python3 -c "
import json;r=json.load(open('$P/score-out/$n/evaluation_report.json'))
s=r.get('score') or {}
print('$n', r['status'], s.get('wirelength_score'), s.get('congestion_score'), s.get('thermal_score'))"
done
```

### 7.2 读完整日志（含逐格溢出明细）

```
/home/termer22/1_workspace/8_EDA/score-out/logrun/full.log
```

格式：`[INFO] over:<溢出量> cap:<容量>...col:<列> row:<行> layer:<层名>`
注意：日志只打印**最严重的一批**（本例 51 条），而报告里的 `overflow_entries` 是 358 —— 别把日志条数当总数。

### 7.3 可视化

`E:\tmp\EDA\cpp_router\output\congestion_explained.html`（WSL: `/mnt/e/tmp/EDA/cpp_router/output/congestion_explained.html`）
单文件、零依赖，浏览器直接打开。11 步动画讲解 §5.1 / §5.5 的完整因果链，`#s=N` 可定位到第 N 步。

---

## 8. 环境改动记录（上一任助手做的）

1. **修复了 `E:\tmp\EDA\cpp_router` 的 Windows 文件权限**：会话开始时所有 shell 命令都失败（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(...)`），原因是该目录对当前账户缺少「写入所有者」权限。已备份后补上完全控制。备份、体检报告与回滚脚本在 `E:\tmp\EDA\acl-recovery\`：
   ```powershell
   pwsh -NoProfile -File 'E:\tmp\EDA\acl-recovery\acl-backup-3e7adebbbf1b46118140dc3a5c36ba4e.json.ps1' -Path 'E:\tmp\EDA\cpp_router' -AllowRoot 'E:\tmp\EDA\cpp_router' -Restore 'E:\tmp\EDA\acl-recovery\acl-backup-3e7adebbbf1b46118140dc3a5c36ba4e.json'
   ```
2. `chmod +x /home/termer22/1_workspace/8_EDA/score`（原来没有执行位）。
3. 新建 `/home/termer22/1_workspace/8_EDA/score-out/`（历次评测结果）与 `pin_positions.json`、`net_to_die.json`（评分器缓存）。不需要可整体删除。
4. `/tmp/noobs.lef`、`/tmp/no11.lef`、`/tmp/no12.lef`、`/tmp/no1112.lef` —— §5.5 对照实验用的改版 LEF，可删。

---

## 9. 给接手者的三条提醒

1. **先读 §3.3 和 §3.4**，那两条坑（绝对路径、退出码）会让你白白浪费几轮。
2. **不要重做 §5 的验证**。容量模型、拥塞归因、官方 guide 违法、mock 报告 —— 这四件事结论已经钉死，重做只是烧 token。
3. **把力气放在线长上**。当前 68.33 分里，拥塞和热基本到顶，线长从 74.43 提到 95 能带来约 +9.3 分，是唯一的大头。
