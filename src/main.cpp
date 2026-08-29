#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <secp256k1.h>

#include "backend.h"
#include "crypto.h"
#include "hwdetect.h"

namespace {

RunState* gState = nullptr;

void onSigint(int) {
    if (gState) gState->stop.store(true);
}

struct Options {
    int minLen = 5;
    unsigned int threads = 0;
    uint64_t maxAttempts = 0;
    std::string output = "tron_vanity_matches.txt";
    std::string backend = "auto";   // auto | cpu | gpu
    double benchSeconds = 2.0;
    uint32_t keysPerItem = 1;       // GPU: 每 work-item 处理的私钥数
    uint32_t ecWindow = 0;          // GPU: 固定基点窗口位宽（0=用内置默认）
    uint32_t montN = 0;             // GPU: Montgomery 批量求逆 work-group 大小（0=默认）
    bool verbose = false;
    bool listOnly = false;
};

void printUsage() {
    std::cout <<
        "TRON 靓号地址生成器 (CPU + 集成显卡自动检测)\n"
        "规则: 地址结尾 >=N 位相同字符(AAAAA) 或 >=N 位连续号码(12345/54321) 即命中并保存。\n\n"
        "用法: tron_vanity_generator [选项]\n"
        "  --min N          最小匹配位数，默认 5\n"
        "  --threads N      CPU 线程数，默认=逻辑核心数\n"
        "  --max N          最大尝试次数，0=无限(默认)\n"
        "  --output FILE    结果文件，默认 tron_vanity_matches.txt\n"
        "  --backend X      auto | cpu | gpu，默认 auto (按压测算力自动选择)\n"
        "  --bench-seconds S 自动选择时每个后端的压测秒数，默认 2\n"
        "  --keys-per-item N GPU 每个 work-item 连续处理的私钥数，默认 1\n"
        "  --ec-window N     GPU 固定基点窗口位宽 (1=逐bit, 2..8=comb)，默认 7\n"
        "  --mont-n N        GPU Montgomery 批量求逆 work-group 大小 (1=关)，默认 1\n"
        "  --verbose        输出进度\n"
        "  --list           只打印硬件检测结果后退出\n"
        "  --bench [秒]     GPU 压测：扫 keys-per-item 甜点位 + 各尾号规则吞吐\n"
        "  --profile [秒]   GPU 内核逐阶段耗时占比\n"
        "  --gputest        GPU 内核逐项对照 CPU\n"
        "  --selftest / --hashtest   地址算法 / 哈希 自检\n"
        "  --help           显示本帮助\n";
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "缺少参数: " << what << "\n"; return ""; }
            return argv[++i];
        };
        if (a == "--min") o.minLen = std::stoi(next("--min"));
        else if (a == "--threads") o.threads = static_cast<unsigned int>(std::stoul(next("--threads")));
        else if (a == "--max") o.maxAttempts = std::stoull(next("--max"));
        else if (a == "--output") o.output = next("--output");
        else if (a == "--backend") o.backend = next("--backend");
        else if (a == "--bench-seconds") o.benchSeconds = std::stod(next("--bench-seconds"));
        else if (a == "--keys-per-item") o.keysPerItem = static_cast<uint32_t>(std::stoul(next("--keys-per-item")));
        else if (a == "--ec-window") o.ecWindow = static_cast<uint32_t>(std::stoul(next("--ec-window")));
        else if (a == "--mont-n") o.montN = static_cast<uint32_t>(std::stoul(next("--mont-n")));
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--list") o.listOnly = true;
        else if (a == "--help" || a == "-h") { printUsage(); return false; }
        else { std::cerr << "未知参数: " << a << "\n"; printUsage(); return false; }
    }
    if (o.minLen < 2) { std::cerr << "--min 至少为 2\n"; return false; }
    return true;
}

class FileSink {
public:
    explicit FileSink(std::string path) : path_(std::move(path)) {}

    void operator()(const FoundKey& fk) {
        std::lock_guard<std::mutex> lk(mu_);
        bool fresh = !std::ifstream(path_).good();
        std::ofstream out(path_, std::ios::app | std::ios::binary);
        if (out.is_open()) {
            if (fresh) out << "\xEF\xBB\xBF";  // UTF-8 BOM，方便 Windows 记事本识别
            out << "类型: " << fk.match.kind << " (" << fk.match.runLen << " 位)\n"
                << "结尾: " << fk.match.tail << "\n"
                << "地址: " << fk.address << "\n"
                << "私钥: " << fk.privHex << "\n"
                << "----------------------------------------\n";
        } else {
            std::cerr << "无法写入 " << path_ << "\n";
        }
        std::cout << "[命中 #" << (++shown_) << "] " << fk.match.kind << " "
                  << fk.match.runLen << " 位  结尾=" << fk.match.tail
                  << "  地址=" << fk.address << "\n";
    }

private:
    std::string path_;
    std::mutex mu_;
    uint64_t shown_ = 0;
};

std::string humanRate(double r) {
    char buf[64];
    if (r >= 1e6) std::snprintf(buf, sizeof(buf), "%.2f M keys/s", r / 1e6);
    else if (r >= 1e3) std::snprintf(buf, sizeof(buf), "%.1f K keys/s", r / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.0f keys/s", r);
    return buf;
}

std::string groupThousands(uint64_t v) {
    std::string s = std::to_string(v), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++c;
    }
    return std::string(out.rbegin(), out.rend());
}

std::string keysPerSec(double r) { return groupThousands(static_cast<uint64_t>(r + 0.5)) + " keys/s"; }

}  // namespace

int selftest() {
    // privkey = 0x00..01 的已知 TRON 地址
    unsigned char sk[32] = {0};
    sk[31] = 1;
    secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    secp256k1_pubkey pub;
    secp256k1_ec_pubkey_create(c, &pub, sk);
    unsigned char out[65];
    size_t len = 65;
    secp256k1_ec_pubkey_serialize(c, out, &len, &pub, SECP256K1_EC_UNCOMPRESSED);
    secp256k1_context_destroy(c);
    std::string addr = tronAddressFromPubXY(out + 1);
    std::cout << "privkey=1 -> " << addr << "\n";
    const std::string expect = "TMVQGm1qAQYVdetCeGRRkTWYYrLXuHK2HC";
    std::cout << (addr == expect ? "SELFTEST OK\n" : "SELFTEST MISMATCH (expected " + expect + ")\n");
    return addr == expect ? 0 : 1;
}

int hashtest();
int gpuSelfTest(const GpuDevice& dev);
int gpuBench(const GpuDevice& dev, double secs);
int gpuProfile(const GpuDevice& dev, double secs);
void gpuSetKeysPerItem(uint32_t n);
void gpuSetEcWindow(uint32_t w);
void gpuSetMontN(uint32_t n);

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") return selftest();
        if (a == "--hashtest") return hashtest();
        if (a == "--gputest") {
            HardwareReport hw = detectHardware();
            if (hw.gpus.empty()) { std::cerr << "无 GPU: " << hw.openclNote << "\n"; return 1; }
            return gpuSelfTest(hw.gpus[0]);
        }
        if (a == "--bench" || a == "--profile") {
            HardwareReport hw = detectHardware();
            if (hw.gpus.empty()) { std::cerr << "无 GPU: " << hw.openclNote << "\n"; return 1; }
            double secs = (i + 1 < argc) ? std::atof(argv[i + 1]) : 2.0;
            if (secs <= 0) secs = 2.0;
            return a == "--profile" ? gpuProfile(hw.gpus[0], secs) : gpuBench(hw.gpus[0], secs);
        }
    }

    Options opt;
    if (!parseArgs(argc, argv, opt)) return 0;

    gpuSetKeysPerItem(opt.keysPerItem);
    gpuSetEcWindow(opt.ecWindow);
    gpuSetMontN(opt.montN);
    HardwareReport hw = detectHardware();

    // 候选后端
    std::vector<std::unique_ptr<Backend>> backends;
    if (opt.backend == "cpu" || opt.backend == "auto")
        backends.push_back(makeCpuBackend());
    if (opt.backend == "gpu" || opt.backend == "auto") {
        for (const auto& g : hw.gpus) backends.push_back(makeGpuBackend(g));
        if (hw.gpus.empty() && opt.backend == "gpu") {
            std::cerr << "未检测到可用 GPU (" << hw.openclNote << ")，回退到 CPU。\n";
            backends.push_back(makeCpuBackend());
        }
    }

    auto line = [] { std::cout << std::string(44, '-') << "\n"; };
    std::cout << "\n";
    line();
    std::cout << "        TRON VANITY GENERATOR\n";
    line();
    std::cout << "\n";

    struct Cand { Backend* b; double rate; bool ok; };
    std::vector<Cand> cands;
    bool doBench = !opt.listOnly && backends.size() > 1;

    for (auto& up : backends) {
        Backend* b = up.get();
        BackendInfo bi = b->info();
        std::cout << bi.kind << "  : " << bi.title << "\n";
        for (auto& l : bi.lines) std::cout << "       " << l << "\n";

        double rate = 0.0;
        bool ok = b->available();
        if (!ok) {
            std::cout << "       [" << b->note() << "]\n";
        } else if (doBench) {
            rate = b->benchmark(opt.benchSeconds);
            std::cout << "       " << keysPerSec(rate) << "\n";
        }
        cands.push_back({b, rate, ok});
        std::cout << "\n";
    }

    if (hw.gpus.empty() && opt.backend != "cpu") {
        std::cout << "GPU  : 无可用 OpenCL GPU\n       [" << hw.openclNote << "]\n\n";
    }

    if (opt.listOnly) return 0;

    // 选择最快的可用后端
    Backend* chosen = nullptr;
    double chosenRate = 0.0;
    for (auto& c : cands) {
        if (c.ok && (!chosen || c.rate > chosenRate)) { chosen = c.b; chosenRate = c.rate; }
    }
    if (!chosen) { std::cerr << "没有可用的计算后端。\n"; return 1; }

    // 组织 Reason 文案
    std::string reason;
    if (doBench) {
        for (auto& c : cands) {
            if (c.b == chosen || !c.ok) continue;
            reason = chosen->name() + " faster (" + c.b->name() + " " + keysPerSec(c.rate) + ")";
        }
    }
    for (auto& c : cands) {
        if (!c.ok && chosen->info().kind == "CPU")
            reason = c.b->name() + " 不可用: " + c.b->note();
    }

    line();
    std::cout << "Backend : " << chosen->name() << "\n";
    if (!reason.empty()) std::cout << "Reason  : " << reason << "\n";
    if (doBench) std::cout << "Speed   : " << keysPerSec(chosenRate) << "\n";
    line();
    std::cout << "\n";

    RunConfig cfg;
    cfg.minLen = opt.minLen;
    cfg.threads = opt.threads;
    cfg.maxAttempts = opt.maxAttempts;
    cfg.verbose = opt.verbose;

    RunState state;
    gState = &state;
    std::signal(SIGINT, onSigint);

    std::cout << "最小位数 : " << opt.minLen << "\n"
              << "输出文件 : " << opt.output << "\n"
              << "尝试上限 : " << (opt.maxAttempts ? groupThousands(opt.maxAttempts) : "无限") << "\n"
              << "按 Ctrl+C 停止。\n\n";

    FileSink sink(opt.output);
    ReportFn report = [&sink](const FoundKey& fk) { sink(fk); };

    std::atomic<bool> done{false};
    auto start = std::chrono::steady_clock::now();

    const int reportEvery = opt.verbose ? 5 : 30;
    std::thread progress([&] {
        int elapsed = 0;
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (done.load()) break;
            if (++elapsed % (reportEvery * 4) != 0) continue;
            double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            uint64_t c = state.checked.load();
            std::cout << "已检查 " << groupThousands(c) << " (" << keysPerSec(el > 0 ? c / el : 0)
                      << ")，命中 " << state.found.load() << "\n";
        }
    });

    chosen->run(cfg, state, report);
    done.store(true);
    progress.join();

    double el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    uint64_t total = state.checked.load();
    std::cout << "\n";
    line();
    std::cout << "Backend    : " << chosen->name() << "\n"
              << "Total keys : " << groupThousands(total) << "\n"
              << "Elapsed    : " << (long)el << " s\n"
              << "Keys/s     : " << keysPerSec(el > 0 ? total / el : 0) << "\n"
              << "Matches    : " << state.found.load() << "\n";
    line();
    return 0;
}
