# TRON Vanity Generator

CPU + Intel/AMD 集成显卡自动检测、按算力自动选择后端，Windows 本机运行的 TRON 靓号地址生成器。

命中规则：Base58 地址**结尾**满足以下任一条件即写入结果文件：

- `>= N` 位**相同字符**，如 `...AAAAA`
- `>= N` 位**连续号码**（按 Base58 字母表递增或递减），如 `...12345` / `...54321`

`N` 由 `--min` 指定，默认 5。位数更多的匹配优先记录。

## 构建（已安装的 VS2026 生成工具）

无需手动打开“开发者命令提示符”，普通 PowerShell 即可：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本会：定位 VS → 若缺 `third_party/secp256k1` 则自动 `git clone` →
用 VS 自带的 CMake + Ninja 配置并编译 → 产物 `build\tron_vanity_generator.exe`。

依赖全部**随源码自带或自动获取**：
- `third_party/secp256k1`（bitcoin-core/libsecp256k1，`build.ps1` 自动克隆）
- SHA-256 已内置（`src/crypto.cpp`），随机数用 Windows `BCryptGenRandom`
- 无需 OpenSSL

也可手动：
```powershell
cmake -B build -G Ninja
cmake --build build --config Release
```

## 使用

```powershell
.\build\tron_vanity_generator.exe --list          # 只看硬件检测
.\build\tron_vanity_generator.exe                  # 自动选后端，无限跑，Ctrl+C 停止
.\build\tron_vanity_generator.exe --min 6 --verbose
.\build\tron_vanity_generator.exe --backend cpu --threads 12 --output result.txt
.\build\tron_vanity_generator.exe --selftest       # 用已知私钥校验地址算法
.\build\tron_vanity_generator.exe --hashtest       # 校验 SHA-256 / Keccak-256
.\build\tron_vanity_generator.exe --gputest        # GPU 内核逐项对照 CPU
.\build\tron_vanity_generator.exe --bench 4         # 扫 keys-per-item + 规则吞吐
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

**逐阶段剖析** `--profile [秒]`：消融法（每阶段单独编译内核）估算内核各阶段耗时占比。
本机 UHD 730 实测：

| 阶段 | 占比 |
| --- | --- |
| EC 标量乘（≤32 点加，`P0 + base·G`） | **~61%** |
| 模逆 + 转仿射 | ~32% |
| Keccak-256 / SHA-256d / Base58 / 匹配 | 合计 ~7% |

→ 瓶颈是**标量乘 + 模逆**（占 93%），哈希和 base58 不值得再碰。
下一步：Montgomery 批量求逆（一次求逆处理 N 个点）+ 增量 `+G` 同时摊薄这两项。

实测（本机 i5-12400 + Intel UHD 730，稳态）：

| 后端 | keys/s |
| --- | --- |
| CPU (12 线程) | ≈ 30 万 |
| GPU (UHD 730, 24 EU) | ≈ 50 万 |

`auto` 选 GPU，60 秒实跑稳定在 ~50 万/s。尾号规则对吞吐几乎无影响（都在 51–52 万）。
`--keys-per-item` 在 UHD 730 上 N=1 最快（每私钥仍需一次模逆，N>1 只增加寄存器压力）；
换独显/更强核显可 `--bench` 重新找甜点位。

后续可优化：**Montgomery 批量求逆**（一次求逆 + 3(N−1) 次乘法，才能让 N>1 真正划算）、
local memory 缓存预计算表、`CL_MEM_ALLOC_HOST_PTR` 零拷贝、双缓冲流水线。

## 安全提示

- 私钥以明文十六进制写入结果文件，请妥善保管，生成后尽快转移资产。
- 请在可信、无恶意软件的机器上运行。
