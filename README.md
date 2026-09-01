# TRON Vanity Generator

High-performance TRON (TRX) vanity address generator for Windows.

- CPU multi-threaded generation (libsecp256k1)
- Intel / AMD **OpenCL GPU** backend — secp256k1 + Keccak-256 + SHA-256 + Base58 + suffix matching all in-kernel
- Automatic hardware detection, automatic CPU/GPU benchmark and **fastest-backend selection**
- Suffix matching: repeated characters (`AAAAA`) and ascending sequences (`12345`, `abcde`, `WXYZ`)
- GPU ↔ CPU cross-validation (`--gputest`), rule self-check (`--matchtest`)
- No OpenSSL. Single self-contained `.exe` (static runtime), only needs a GPU driver's `OpenCL.dll` for the GPU path

Key generation uses the OS CSPRNG (`BCryptGenRandom`) and libsecp256k1 for elliptic-curve operations.

## Quick Start

```powershell
git clone --recurse-submodules https://github.com/hlzzhqb/tron-vanity-generator.git
cd tron-vanity-generator
powershell -ExecutionPolicy Bypass -File build.ps1

.\build\tron_vanity_generator.exe --selftest
.\build\tron_vanity_generator.exe --gputest
.\build\tron_vanity_generator.exe               # auto-select backend, run until Ctrl+C
```

Or grab `tron_vanity_generator.exe` from [Releases](https://github.com/hlzzhqb/tron-vanity-generator/releases) — no build, no VC++ runtime needed.

> ⚠️ Private keys of matches are written **in plaintext** to `tron_vanity_matches.txt` (gitignored).
> Never commit or upload result files. Move funds off generated addresses promptly.

---

CPU + Intel/AMD 集成显卡自动检测、按算力自动选择后端，Windows 本机运行的 TRON 靓号地址生成器。

命中规则：Base58 地址**结尾**满足以下任一条件即写入结果文件：

- `>= N` 位**相同字符**，如 `...AAAAA`
- `>= N` 位**连续字符**：**只认升序**——每位字符的 ASCII 值 `+1`，且整段落在**同一类别**内
  （全数字 / 全小写 / 全大写）。如 `...12345`、`...abcde`、`...WXYZ`
  - **不算**连续：`54321` / `edcba`（降序）、`89123`（跨过 base58 里没有的 `0`）、
    `xyzab`（`z→a` 回绕）、`9abcd`（数字跨到字母）、`FGHJ`（跳过 base58 里没有的 `I`）

`N` 由 `--min` 指定，默认 5。位数更多的匹配优先记录。`--matchtest` 可自检规则。

## 构建（Windows，Visual Studio / Build Tools）

无需手动打开“开发者命令提示符”，普通 PowerShell 即可：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本会：定位 VS → 拉取 `third_party/secp256k1` 子模块 →
用 VS 自带的 CMake + Ninja 配置并编译 → 产物 `build\tron_vanity_generator.exe`。

依赖：
- `third_party/secp256k1`（bitcoin-core/libsecp256k1，git 子模块，MIT）
- SHA-256 / Keccak-256 已内置（`src/`），随机数用 Windows `BCryptGenRandom`
- 无需 OpenSSL。GPU 用运行期动态加载的 `OpenCL.dll`（显卡驱动自带）

## 复制到另一台机器运行

生成的 exe **静态链接运行库**，只依赖 `KERNEL32.dll` + `bcrypt.dll`（Win10/11 自带），
不需要装 VC++ 运行时。内核已嵌进 exe，secp256k1 也静态链接。

**方式 A — 只拷 exe（最快，目标机也是 64 位 Windows）**
1. 把 `build\tron_vanity_generator.exe` 拷到目标机任意目录
2. 直接运行。`--backend cpu` / `auto` 立即可用
3. 想用 GPU：目标机需装 Intel/AMD 显卡驱动（自带 `OpenCL.dll`）。没有就自动回退 CPU
4. 到目标机先跑一次 `tron_vanity_generator.exe --selftest` 和 `--gputest` 确认无误

**方式 B — 拷源码在目标机重新编译**
1. 拷整个文件夹（`build\` 和 `third_party\` 可不拷）
2. 目标机装 Visual Studio 2022/2026 或 Build Tools（勾选“使用 C++ 的桌面开发”）+ git
3. `powershell -ExecutionPolicy Bypass -File build.ps1`（联网，会自动 clone secp256k1）
   - 无网络时：先在有网机器 `git clone --depth 1 https://github.com/bitcoin-core/secp256k1 third_party/secp256k1`，连 `third_party\` 一起拷过去

私钥文件（`tron_vanity_matches.txt`）是明文，拷贝/传输时注意安全。

## 使用

```powershell
.\build\tron_vanity_generator.exe --list          # 只看硬件检测
.\build\tron_vanity_generator.exe                  # 自动选后端，无限跑，Ctrl+C 停止
.\build\tron_vanity_generator.exe --min 6 --verbose
.\build\tron_vanity_generator.exe --backend cpu --threads 12 --output result.txt
.\build\tron_vanity_generator.exe --selftest       # 用已知私钥校验地址算法
.\build\tron_vanity_generator.exe --hashtest       # 校验 SHA-256 / Keccak-256
.\build\tron_vanity_generator.exe --gputest        # GPU 内核逐项对照 CPU
.\build\tron_vanity_generator.exe --bench 4         # 扫 ECW / MONT_N / KPI + 规则吞吐
.\build\tron_vanity_generator.exe --profile 4       # 内核逐阶段耗时占比
```

| 参数 | 说明 |
| --- | --- |
| `--min N` | 最小匹配位数，默认 5 |
| `--threads N` | CPU 线程数，默认=逻辑核心数 |
| `--max N` | 最大尝试次数，0=无限（默认） |
| `--output FILE` | 结果文件，默认 `tron_vanity_matches.txt`（UTF-8 BOM） |
| `--backend auto\|cpu\|gpu` | 默认 `auto`：压测各后端算力后选最快 |
| `--bench-seconds S` | 自动选择时每个后端压测时长，默认 2 |
| `--verbose` | 输出进度 |
| `--list` | 打印硬件检测后退出 |

## 硬件检测与自动选择

- **CPU**：CPUID 读型号 / AES-NI / AVX2，`hardware_concurrency` 取逻辑核数。
- **GPU**：运行期动态加载 `OpenCL.dll`（Intel/AMD 显卡驱动自带，无需 SDK），
  枚举各 OpenCL 平台的 GPU，读取 CU 数、频率、显存，用
  `CL_DEVICE_HOST_UNIFIED_MEMORY` 判断集成显卡。
- **自动选择**：对每个可用后端跑一次压测（keys/sec），选吞吐最高的。

## 实现进度

| 阶段 | 状态 |
| --- | --- |
| 硬件检测（CPU CPUID + OpenCL GPU 枚举） | ✅ |
| 报告 UI + 自动算力压测 + `Backend / Reason / Speed` 选择 | ✅ |
| CPU 后端：libsecp256k1，多线程 | ✅ |
| Keccak-256 / SHA-256 / Base58Check / 尾号匹配（`--selftest` / `--hashtest`） | ✅ |
| **GPU 后端：OpenCL secp256k1 批处理内核** | ✅ |
| GPU↔CPU 交叉验证（`--gputest`，含 s=0 / 2³¹−1 / 2³²−1 等边界） | ✅ 完全一致 |

GPU 数据流：host 上传基点 `P0 = k0·G` 与预计算表 `T[j] = 2^j·G` → 每个 work-item 用
≤32 次雅可比点加算出 `P0 + s·G` → 在**内核内**完成 keccak256 / sha256d / base58 尾部 /
尾号判定 → 只 `atomic_inc` 回传命中的 `s` → host 用 libsecp256k1 复算确认后写文件。
每次内核启动扫 2²⁰ 个连续私钥，随机换 `k0`。域/群运算移植自 bitcoin-core/libsecp256k1
（`field_10x26`），费马求逆用其加法链。base58 尾部直接输出字符**位置**(0–57)，
判定“相同/连续”无需逆查表。

**基准测试** `--bench [秒]`：每档预热到稳态再计时，扫 `--keys-per-item` 甜点位，
并按尾号规则（相同/连续 ≥ 5/6/7/8 位）分别测吞吐。

**逐阶段剖析** `--profile [秒]`：消融法（每阶段单独编译内核）估算各阶段耗时占比。

**固定基点窗口法** `--ec-window N`（默认 7）：基点是固定的 `G`，标量乘改用 comb 预计算表
`T[w][d] = d·2^(w·N)·G`，点加次数从 ~10 降到 `ceil(20/N)`。UHD 730 实测 `--bench`：

| ECW | 点加 | 表 | keys/s | vs baseline |
| --- | --- | --- | --- | --- |
| 1（逐 bit，baseline） | ~10 | 2 KB | 519K | — |
| 4 | 5 | 5 KB | 747K | +44% |
| 7（默认） | 3 | 24 KB | 820K | **+58%** |
| 8 | 3 | 48 KB | 823K | +59%（表翻倍收益 <1%）|

profile 随之变化（UHD 730，ECW=7）：

| 阶段 | baseline | ECW=7 |
| --- | --- | --- |
| EC 标量乘 | ~61% (1200 ns) | ~35% (431 ns) |
| **模逆 + 转仿射** | ~32% (636 ns) | **~55% (672 ns)** |
| Keccak / SHA / Base58 / 匹配 | ~7% | ~10% |
| 合计 | 1961 ns/key (508K) | **1227 ns/key (815K)** |

→ ECW=7 后瓶颈是**模逆**（55%）。

**Montgomery 批量求逆** `--mont-n N`（已实现，默认关）：数据布局按「1 work-item = 1 私钥 =
1 Jacobian 点」，work-group 内 N 个 Z 坐标 → `lid==0` 做前缀积 + **一次** `fe_inv` + 后缀恢复
→ N 个 `z⁻¹` → 各 WI 转仿射。`--gputest` 对照 libsecp256k1 验证 N=2/4/8/16/32 全部一致。

**但 UHD 730 实测收益 ≈ 0**（`--bench`）：

| MONT_N | keys/s | vs N=1 |
| --- | --- | --- |
| 1（每 WI 各自求逆） | ~800–826K | — |
| 2 | ~128K | **−84%** |
| 4 / 8 | ~250K / ~468K | −69% / −42% |
| 16 | ~842K | +5%（噪声级）|
| 32 | ~763K | −5% |

原因：`lid==0` 串行做整个 `fe_inv`（~255 次平方的长依赖链），其余 N−1 个 lane 卡在 barrier；
小 N 时这个停顿吃掉一切，大 N 才勉强摊平。而 N=1 时每个 lane 各自求逆本来就**完全并行**
（SIMT 用几千个 work-item 掩盖单次求逆的延迟）——Montgomery 把「多个并行求逆」换成
「一次串行求逆 + 停顿」，在这种海量并行的场景是亏的。这正是「理论复杂度↓ → occupancy↓ →
实际变慢」的典型案例。`--mont-n` 保留给占用特性不同的其它 GPU。

实测（**单一样本**：i5-12400 + Intel UHD 730，稳态）：

| 阶段 | CPU | GPU (60s 实跑) | 倍数 |
| --- | --- | --- | --- |
| baseline | ~305K | ~500K | 1.7× |
| + 固定基点窗口法 | ~305K | ~785K | 2.6× |
| + Montgomery（默认关，此卡无收益） | ~300K | **~820K** | **2.7×** |

> 吞吐随 GPU 型号 / 驱动 / OpenCL 实现 / 运行参数差异很大，UHD 730 也不保证普遍能到 820K/s。
> **换硬件请自己跑 `--bench`** 找 ECW / KPI / MONT_N 甜点位。

`auto` 选 GPU。尾号规则对吞吐无影响。`--keys-per-item` / `--mont-n` 在 UHD 730 上都是 N=1 最快
（见上）；换 GPU 用 `--bench` 重新找 ECW / KPI / MONT_N 甜点位。

500K → 820K 的提升**全部来自固定基点窗口法**；Montgomery 在这块 iGPU 上没帮助。
更强的独显可能是另一套最优参数——`--bench` 会自己告诉你。

## 安全提示

- 私钥以明文十六进制写入结果文件（`tron_vanity_matches.txt`，已在 `.gitignore` 中），
  请妥善保管，生成后尽快转移资产。
- 随机数用操作系统 CSPRNG（`BCryptGenRandom`）+ libsecp256k1，无自造弱随机。
- 请在可信、无恶意软件的机器上运行。审阅代码后自行编译最稳妥。

## License

[MIT](LICENSE) © 2026 Huang Qingbiao。子模块 `third_party/secp256k1` 亦为 MIT。
