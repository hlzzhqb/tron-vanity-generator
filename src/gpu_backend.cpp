#include "backend.h"
#include "crypto.h"
#include "hwdetect.h"
#include "ocl.h"
#include "rng.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <secp256k1.h>

#include "kernel_src.h"

namespace {

constexpr uint32_t kBatch = 1u << 20;   // 每次内核启动扫描的连续私钥数
constexpr uint32_t kOutCap = 8192;      // 单批命中回传上限

// 每个 work-item 连续处理多少私钥。UHD 730 上实测 N=1 最快（寄存器压力 + 每个私钥仍需一次
// 模逆，未做 Montgomery 批量求逆）。保留可调，供不同 GPU 找甜点位。
uint32_t g_keysPerItem = 1;

bool pubXY(secp256k1_context* c, const unsigned char sk[32], unsigned char out64[64]) {
    secp256k1_pubkey p;
    if (!secp256k1_ec_pubkey_create(c, &p, sk)) return false;
    unsigned char o[65];
    size_t l = 65;
    secp256k1_ec_pubkey_serialize(c, o, &l, &p, SECP256K1_EC_UNCOMPRESSED);
    std::memcpy(out64, o + 1, 64);
    return true;
}

void scalarBE(uint32_t s, unsigned char out[32]) {
    std::memset(out, 0, 32);
    out[28] = static_cast<unsigned char>(s >> 24);
    out[29] = static_cast<unsigned char>(s >> 16);
    out[30] = static_cast<unsigned char>(s >> 8);
    out[31] = static_cast<unsigned char>(s);
}

class GpuBackend : public Backend {
public:
    explicit GpuBackend(GpuDevice hw) : hw_(std::move(hw)) {
        ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    }

    std::string name() const override { return "OpenCL GPU"; }

    BackendInfo info() const override {
        BackendInfo bi;
        bi.kind = "GPU";
        bi.title = hw_.name;
        bi.lines.push_back("OpenCL  " + hw_.platform);
        bi.lines.push_back(std::to_string(hw_.computeUnits) + " CU");
        bi.lines.push_back(std::to_string(hw_.clockMHz) + " MHz");
        bi.lines.push_back(hw_.integrated ? "集成显卡 (统一内存)" : "独立显卡");
        return bi;
    }

    bool available() const override { return const_cast<GpuBackend*>(this)->ensureReady(); }
    std::string note() const override { return err_; }

    double benchmark(double seconds) override {
        if (!ensureReady()) return 0.0;
        benchN(seconds * 0.6, 20);          // 预热：让集成显卡降到稳态
        return benchN(seconds, 20);
    }

    // 完整流水线压测：secp256k1 + keccak + sha + base58 + 匹配 + 回传
    double benchN(double seconds, uint32_t minLen) {
        if (!ensureReady()) return 0.0;
        auto start = std::chrono::steady_clock::now();
        uint64_t done = 0;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
            if (!runBatch(minLen, nullptr, nullptr)) break;
            done += kBatch;
        }
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return el > 0 ? done / el : 0.0;
    }

    void run(const RunConfig& cfg, RunState& state, const ReportFn& report) override {
        if (!ensureReady()) { std::cerr << "GPU 后端不可用: " << err_ << "\n"; return; }
        while (!state.stop.load(std::memory_order_relaxed)) {
            if (cfg.maxAttempts && state.checked.load() >= cfg.maxAttempts) { state.stop = true; break; }
            std::vector<uint32_t> hits;
            unsigned char k0[32];
            if (!runBatch(static_cast<uint32_t>(cfg.minLen), &hits, k0)) {
                std::cerr << "GPU 批次执行失败\n";
                return;
            }
            for (uint32_t s : hits) {
                unsigned char k[32];
                std::memcpy(k, k0, 32);
                unsigned char tw[32];
                scalarBE(s, tw);
                if (!secp256k1_ec_seckey_tweak_add(ctx_, k, tw)) continue;
                unsigned char pub[64];
                if (!pubXY(ctx_, k, pub)) continue;
                std::string addr = tronAddressFromPubXY(pub);
                MatchResult m = evaluateAddress(addr, cfg.minLen);
                if (!m.matched) continue;   // GPU 误报，丢弃
                FoundKey fk{addr, bytesToHexUpper(k, 32), std::move(m)};
                state.found.fetch_add(1, std::memory_order_relaxed);
                report(fk);
            }
            state.checked.fetch_add(kBatch, std::memory_order_relaxed);
        }
    }

    // ---- 供 --gputest 使用 ----
    static int selfTest(const GpuDevice& hw);

private:
    GpuDevice hw_;
    secp256k1_context* ctx_ = nullptr;
    bool tried_ = false, ready_ = false;
    std::string err_;

    ocl::Program prog_;
    ocl::id kProbe_ = nullptr;
    ocl::id bufTable_ = nullptr, bufP0_ = nullptr, bufCount_ = nullptr, bufOutS_ = nullptr;
    std::vector<unsigned char> table_;   // 32 * 64
    uint32_t builtKpi_ = 8;

    bool ensureReady() {
        if (tried_) return ready_;
        tried_ = true;
        if (!ocl::load(&err_)) return false;
        if (!hw_.deviceId) { err_ = "无 OpenCL 设备句柄"; return false; }
        builtKpi_ = g_keysPerItem ? g_keysPerItem : 1;
        while (kBatch % builtKpi_) --builtKpi_;
        std::string opts = "-D KPI=" + std::to_string(builtKpi_);
        if (!prog_.build(hw_.platformId, hw_.deviceId, kGpuKernelSource, opts, &err_)) return false;
        kProbe_ = prog_.kernel("tron_vanity_probe", &err_);
        if (!kProbe_) return false;

        // 预计算表 T[j] = 2^j * G
        table_.assign(32 * 64, 0);
        for (int j = 0; j < 32; ++j) {
            unsigned char sk[32] = {0};
            sk[31 - j / 8] = static_cast<unsigned char>(1u << (j % 8));
            if (!pubXY(ctx_, sk, &table_[j * 64])) { err_ = "预计算表生成失败"; return false; }
        }
        bufTable_ = prog_.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, table_.size(), table_.data(), &err_);
        bufP0_ = prog_.buffer(ocl::MEM_READ_ONLY, 64, nullptr, &err_);
        bufCount_ = prog_.buffer(ocl::MEM_READ_WRITE, 4, nullptr, &err_);
        bufOutS_ = prog_.buffer(ocl::MEM_WRITE_ONLY, kOutCap * 4, nullptr, &err_);
        if (!bufTable_ || !bufP0_ || !bufCount_ || !bufOutS_) return false;

        ready_ = true;
        return true;
    }

    // 跑一批：随机基私钥 k0，扫描 k0+[0,kBatch)。命中的 s 写入 outHits（可空）。
    bool runBatch(uint32_t minLen, std::vector<uint32_t>* outHits, unsigned char outK0[32]) {
        unsigned char k0[32], p0[64];
        do {
            if (!randBytes(k0, 32)) return false;
        } while (!pubXY(ctx_, k0, p0));
        if (outK0) std::memcpy(outK0, k0, 32);

        if (!prog_.write(bufP0_, 64, p0)) return false;
        uint32_t zero = 0;
        if (!prog_.write(bufCount_, 4, &zero)) return false;

        uint32_t global = kBatch / builtKpi_;
        if (!prog_.setArg(kProbe_, 0, sizeof(ocl::id), &bufP0_)) return false;
        prog_.setArg(kProbe_, 1, sizeof(ocl::id), &bufTable_);
        prog_.setArg(kProbe_, 2, sizeof(uint32_t), &minLen);
        prog_.setArg(kProbe_, 3, sizeof(ocl::id), &bufCount_);
        prog_.setArg(kProbe_, 4, sizeof(ocl::id), &bufOutS_);
        uint32_t cap = kOutCap;
        prog_.setArg(kProbe_, 5, sizeof(uint32_t), &cap);

        if (!prog_.run1D(kProbe_, global, 0, &err_)) return false;
        if (!prog_.finish()) return false;

        if (outHits) {
            uint32_t cnt = 0;
            prog_.read(bufCount_, 4, &cnt);
            if (cnt > kOutCap) cnt = kOutCap;
            outHits->resize(cnt);
            if (cnt) prog_.read(bufOutS_, cnt * 4, outHits->data());
        }
        return true;
    }
};

int GpuBackend::selfTest(const GpuDevice& hw) {
    std::string err;
    if (!ocl::load(&err)) { std::cout << "OpenCL 加载失败: " << err << "\n"; return 1; }
    ocl::Program prog;
    if (!prog.build(hw.platformId, hw.deviceId, kGpuKernelSource, "", &err)) {
        std::cout << err << "\n";
        return 1;
    }
    ocl::id kTest = prog.kernel("test_pub", &err);
    if (!kTest) { std::cout << err << "\n"; return 1; }

    secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);

    std::vector<unsigned char> table(32 * 64, 0);
    for (int j = 0; j < 32; ++j) {
        unsigned char sk[32] = {0};
        sk[31 - j / 8] = static_cast<unsigned char>(1u << (j % 8));
        pubXY(c, sk, &table[j * 64]);
    }
    unsigned char k0[32], p0[64];
    do { randBytes(k0, 32); } while (!pubXY(c, k0, p0));

    std::vector<uint32_t> scalars = {0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 255, 256,
                                     1000, 65535, 65536, 123456, 7777777, 0x7FFFFFFF, 0xFFFFFFFF};
    uint32_t n = static_cast<uint32_t>(scalars.size());
    std::vector<unsigned char> pubout(n * 64, 0);

    ocl::id bT = prog.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, table.size(), table.data(), &err);
    ocl::id bP = prog.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, 64, p0, &err);
    ocl::id bS = prog.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, n * 4, scalars.data(), &err);
    ocl::id bO = prog.buffer(ocl::MEM_WRITE_ONLY, n * 64, nullptr, &err);
    prog.setArg(kTest, 0, sizeof(ocl::id), &bP);
    prog.setArg(kTest, 1, sizeof(ocl::id), &bT);
    prog.setArg(kTest, 2, sizeof(ocl::id), &bS);
    prog.setArg(kTest, 3, sizeof(ocl::id), &bO);
    prog.setArg(kTest, 4, sizeof(uint32_t), &n);
    if (!prog.run1D(kTest, n, 0, &err)) { std::cout << err << "\n"; return 1; }
    prog.finish();
    prog.read(bO, n * 64, pubout.data());

    int fails = 0;
    for (uint32_t i = 0; i < n; ++i) {
        unsigned char k[32];
        std::memcpy(k, k0, 32);
        unsigned char tw[32];
        scalarBE(scalars[i], tw);
        unsigned char cpuPub[64];
        bool ok = secp256k1_ec_seckey_tweak_add(c, k, tw) && pubXY(c, k, cpuPub);
        bool match = ok && std::memcmp(cpuPub, &pubout[i * 64], 64) == 0;
        std::string cpuAddr = ok ? tronAddressFromPubXY(cpuPub) : "?";
        std::string gpuAddr = tronAddressFromPubXY(&pubout[i * 64]);
        std::cout << (match ? "  OK  " : "  FAIL") << "  s=" << scalars[i]
                  << "  cpu=" << cpuAddr << "  gpu=" << gpuAddr << "\n";
        if (!match) ++fails;
    }
    std::cout << (fails ? std::to_string(fails) + " 个不一致\n" : "GPU secp256k1/keccak/base58 与 CPU 完全一致\n");
    return fails ? 1 : 0;
}

}  // namespace

std::unique_ptr<Backend> makeGpuBackend(const GpuDevice& dev) {
    return std::make_unique<GpuBackend>(dev);
}

int gpuSelfTest(const GpuDevice& dev) { return GpuBackend::selfTest(dev); }

void gpuSetKeysPerItem(uint32_t n) { if (n) g_keysPerItem = n; }

// --profile：逐阶段消融，估算内核各阶段占比
int gpuProfile(const GpuDevice& dev, double secs) {
    std::string err;
    if (!ocl::load(&err)) { std::cout << err << "\n"; return 1; }
    secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);

    std::vector<unsigned char> table(32 * 64, 0);
    for (int j = 0; j < 32; ++j) {
        unsigned char sk[32] = {0};
        sk[31 - j / 8] = static_cast<unsigned char>(1u << (j % 8));
        pubXY(c, sk, &table[j * 64]);
    }
    unsigned char k0[32], p0[64];
    do { randBytes(k0, 32); } while (!pubXY(c, k0, p0));

    const uint32_t kb = 1u << 20;
    const char* names[7] = { "", "EC 标量乘 (<=32 点加)", "模逆 + 转仿射", "Keccak-256",
                             "SHA-256d (校验和)", "Base58 尾部", "尾号匹配" };
    double nsPerKey[7] = {0};

    double hostNs = 0;

    std::cout << "\n== TRON GPU Profile (" << dev.name << ", KPI=1) ==\n"
              << "逐阶段消融：每阶段单独编译内核，预热到稳态后计时。\n\n";

    for (int stage = 1; stage <= 6; ++stage) {
        ocl::Program p;
        std::string opts = "-D KPI=1 -D PROF_STAGE=" + std::to_string(stage);
        if (!p.build(dev.platformId, dev.deviceId, kGpuKernelSource, opts, &err)) {
            std::cout << "stage " << stage << " 编译失败:\n" << err << "\n";
            return 1;
        }
        ocl::id k = p.kernel("prof", &err);
        ocl::id bT = p.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, table.size(), table.data(), &err);
        ocl::id bPp = p.buffer(ocl::MEM_READ_ONLY | ocl::MEM_COPY_HOST_PTR, 64, p0, &err);
        ocl::id bS = p.buffer(ocl::MEM_WRITE_ONLY, kb * 4, nullptr, &err);
        if (!k || !bT || !bPp || !bS) { std::cout << "缓冲/内核创建失败: " << err << "\n"; return 1; }
        p.setArg(k, 0, sizeof(ocl::id), &bPp);
        p.setArg(k, 1, sizeof(ocl::id), &bT);
        p.setArg(k, 2, sizeof(ocl::id), &bS);

        auto measure = [&](double dur) -> double {
            auto st = std::chrono::steady_clock::now();
            uint64_t done = 0;
            double el;
            do {
                if (!p.run1D(k, kb, 0, &err) || !p.finish()) return 0.0;
                done += kb;
                el = std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count();
            } while (el < dur);
            return el > 0 ? done / el : 0.0;
        };
        measure(secs * 0.6);
        double kps = measure(secs);
        nsPerKey[stage] = kps > 0 ? 1e9 / kps : 0;
        std::printf("  [%d/6] %-22s  %.0f keys/s\n", stage, names[stage], kps);

        if (stage == 6) {
            // host 侧开销：randBytes + pubXY + 上传 P0 + 读回计数（不启动内核）
            const int iters = 300;
            auto st = std::chrono::steady_clock::now();
            for (int i = 0; i < iters; ++i) {
                unsigned char kk[32], pp[64];
                do { randBytes(kk, 32); } while (!pubXY(c, kk, pp));
                p.write(bPp, 64, pp);
                uint32_t tmp;
                p.read(bS, 4, &tmp);
            }
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count();
            hostNs = el / iters / kb * 1e9;
        }
    }

    double total = nsPerKey[6] + hostNs;
    std::cout << "\n阶段                       本阶段 ns/key    占比\n";
    std::cout << "  " << std::string(48, '-') << "\n";
    double prev = 0, maxPct = 0;
    int maxI = 1;
    for (int s = 1; s <= 6; ++s) {
        double d = nsPerKey[s] - prev;
        if (d < 0) d = 0;
        double pct = total > 0 ? d / total * 100 : 0;
        std::printf("  %-24s  %9.0f      %5.1f%%\n", names[s], d, pct);
        if (pct > maxPct) { maxPct = pct; maxI = s; }
        prev = nsPerKey[s];
    }
    {
        double pct = total > 0 ? hostNs / total * 100 : 0;
        std::printf("  %-24s  %9.0f      %5.1f%%\n", "Host <-> Device + 私钥", hostNs, pct);
        if (pct > maxPct) { maxPct = pct; maxI = 0; }
    }
    std::cout << "  " << std::string(48, '-') << "\n";
    std::printf("  合计 ~%.0f ns/key  (~%.0f keys/s)\n", total, total > 0 ? 1e9 / total : 0);
    std::printf("\n结论: 最大头是 %s (~%.0f%%)，优先攻这里。\n",
                maxI == 0 ? "Host<->Device" : names[maxI], maxPct);
    std::cout << "（消融法为近似：加阶段会改变寄存器占用，百分比看量级即可）\n";
    return 0;
}

// --bench：扫 keys-per-item 甜点位 + 按尾号位数看吞吐（每个 N 重新编译内核以获得展开优化）
int gpuBench(const GpuDevice& dev, double secs) {
    std::cout << "\n每档预热 " << secs << "s + 计时 " << secs
              << "s（集成显卡持续负载会降频，取计时段的稳态值）\n";
    std::cout << "\n== keys-per-item 扫描（纯吞吐，min_len=20）==\n";
    uint32_t bestN = 8;
    double bestR = 0;
    for (uint32_t n : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        g_keysPerItem = n;
        GpuBackend b(dev);
        if (!b.available()) { std::cout << "  N=" << n << " 构建失败\n"; continue; }
        b.benchN(secs, 20);                     // 预热（降频到稳态）
        double r = b.benchN(secs, 20);          // 计时
        std::printf("  N=%-3u : %10.0f keys/s%s\n", n, r, r > bestR ? "  <-" : "");
        if (r > bestR) { bestR = r; bestN = n; }
    }
    std::printf("  甜点位 N=%u  (%.0f keys/s)\n", bestN, bestR);

    std::cout << "\n== 尾号规则对吞吐的影响 (N=" << bestN << ") ==\n";
    g_keysPerItem = bestN;
    GpuBackend b(dev);
    b.available();
    b.benchN(secs, 5);
    for (uint32_t ml : {5u, 6u, 7u, 8u}) {
        double r = b.benchN(secs, ml);
        std::printf("  相同/连续 >= %u 位 : %10.0f keys/s\n", ml, r);
    }
    return 0;
}
