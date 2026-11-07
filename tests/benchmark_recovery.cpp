#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

int main() {
    using namespace kvdb;

    const std::string path = "benchmark_1m_keys.data";
    constexpr std::uint32_t kTotalKeys  = 1000000;
    constexpr std::uint32_t kBatchSize  = 10000;
    constexpr std::uint32_t kNumBatches = kTotalKeys / kBatchSize;

    File::remove_file(path);
    File::remove_file(path + ".tmp");

    std::cout << "===================================================\n"
              << "Encrypted Squad - 1,000,000-Key Recovery Benchmark (F3)\n"
              << "===================================================\n"
              << "[1/2] Building fixture with " << kTotalKeys << " distinct keys "
              << "(16-byte keys, 16-byte values)...\n";

    auto build_t0 = std::chrono::steady_clock::now();
    std::uint64_t final_file_bytes = 0;

    {
        Database db;
        Status st = db.open(path);
        if (!st.ok()) {
            std::cerr << "[FAIL] Failed to open fixture DB: " << st.to_string() << "\n";
            return 1;
        }

        std::vector<BatchOp> batch_ops;
        batch_ops.reserve(kBatchSize);

        char key_buf[32];
        char val_buf[32];

        for (std::uint32_t b = 0; b < kNumBatches; ++b) {
            batch_ops.clear();
            std::uint32_t base = b * kBatchSize;
            for (std::uint32_t i = 0; i < kBatchSize; ++i) {
                std::uint32_t id = base + i;
                std::snprintf(key_buf, sizeof(key_buf), "key_%012u", id); // 16 bytes
                std::snprintf(val_buf, sizeof(val_buf), "val_%012u", id); // 16 bytes
                batch_ops.push_back(BatchOp::Put(std::string(key_buf, 16), std::string(val_buf, 16)));
            }
            st = db.batch(batch_ops);
            if (!st.ok()) {
                std::cerr << "[FAIL] Batch write failed at batch " << b << ": " << st.to_string() << "\n";
                return 1;
            }
        }

        final_file_bytes = db.file_size();
        db.close();
    }

    auto build_t1 = std::chrono::steady_clock::now();
    double build_sec = std::chrono::duration<double>(build_t1 - build_t0).count();

    std::cout << "  Fixture created in " << std::fixed << std::setprecision(2) << build_sec << " s\n"
              << "  File size : " << final_file_bytes << " bytes ("
              << (final_file_bytes / (1024.0 * 1024.0)) << " MiB)\n\n"
              << "[2/2] Measuring Database::open() across 5 independent runs (Target: < 2000.00 ms)...\n";

    constexpr int kRuns = 5;
    std::vector<double> times_ms;
    times_ms.reserve(kRuns);

    for (int r = 1; r <= kRuns; ++r) {
        Database db;
        auto t0 = std::chrono::steady_clock::now();
        Status st = db.open(path);
        auto t1 = std::chrono::steady_clock::now();

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::size_t recovered_keys = db.size();
        db.close();

        if (!st.ok() || recovered_keys != kTotalKeys) {
            std::cerr << "[FAIL] Run " << r << " failed: status=" << st.to_string()
                      << " recovered_keys=" << recovered_keys << "\n";
            File::remove_file(path);
            return 1;
        }

        times_ms.push_back(elapsed_ms);
        std::cout << "  Run " << r << ": " << std::fixed << std::setprecision(2) << elapsed_ms
                  << " ms (recovered_keys=" << recovered_keys << ")\n";
    }

    File::remove_file(path);

    std::vector<double> sorted_times = times_ms;
    std::sort(sorted_times.begin(), sorted_times.end());
    double median_ms = sorted_times[kRuns / 2];
    double max_ms    = sorted_times.back();
    double min_ms    = sorted_times.front();

    std::cout << "---------------------------------------------------\n"
              << "  Min    : " << min_ms << " ms\n"
              << "  Median : " << median_ms << " ms\n"
              << "  Max    : " << max_ms << " ms\n"
              << "---------------------------------------------------\n";

    if (max_ms >= 2000.0) {
        std::cerr << "[FAIL] Max recovery time (" << max_ms << " ms) exceeded 2000 ms limit!\n";
        return 1;
    }

    std::cout << "[PASS] All 5 runs recovered 1,000,000 keys in under 2,000 ms!\n";
    return 0;
}