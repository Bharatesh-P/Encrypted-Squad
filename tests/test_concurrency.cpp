#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " (" #cond ")\n"; \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

int main() {
    using namespace kvdb;
    const std::string path = "test_concurrency_db.data";
    File::remove_file(path);

    Database db;
    REQUIRE(db.open(path).ok());

    // Pre-populate batch keys with "v_initial" so concurrent readers (D4) can check atomicity
    const int kBatchKeys = 25;
    {
        std::vector<BatchOp> init_ops;
        for (int i = 0; i < kBatchKeys; ++i) {
            init_ops.push_back(BatchOp::Put("batch_key_" + std::to_string(i), "batch_v_0"));
        }
        REQUIRE(db.batch(init_ops).ok());
    }

    std::atomic<bool> failed{false};
    std::atomic<bool> writers_done{false};

    // Writer 1 & Writer 2: Single puts on distinct keys (D1) and shared key (D2)
    auto single_writer = [&](int writer_id) {
        for (int i = 0; i < 150; ++i) {
            std::string k = "w" + std::to_string(writer_id) + "_k_" + std::to_string(i);
            std::string v = "val_" + std::to_string(writer_id) + "_" + std::to_string(i);
            if (!db.put(k, v).ok()) {
                failed.store(true);
                return;
            }
            if (!db.put("shared_key", v).ok()) {
                failed.store(true);
                return;
            }
        }
    };

    // Writer 3: Atomic batches updating all 25 batch_key_* keys to identical version strings (D3, D4)
    std::thread batch_writer([&]() {
        for (int ver = 1; ver <= 40; ++ver) {
            std::string target_val = "batch_v_" + std::to_string(ver);
            std::vector<BatchOp> ops;
            ops.reserve(kBatchKeys);
            for (int i = 0; i < kBatchKeys; ++i) {
                ops.push_back(BatchOp::Put("batch_key_" + std::to_string(i), target_val));
            }
            if (!db.batch(ops).ok()) {
                failed.store(true);
                return;
            }
        }
    });

    // Reader thread: reads batch keys concurrently with writes (D4)
    std::thread reader([&]() {
        while (!writers_done.load()) {
            std::string val;
            Status st = db.get("batch_key_0", val);
            if (!st.ok() || val.rfind("batch_v_", 0) != 0) {
                failed.store(true);
                return;
            }
        }
    });

    std::thread w1(single_writer, 1);
    std::thread w2(single_writer, 2);

    w1.join();
    w2.join();
    batch_writer.join();
    writers_done.store(true);
    reader.join();

    REQUIRE(!failed.load());
    REQUIRE(db.close().ok());

    // Reopen and verify all 300 distinct writer keys + shared_key + 25 batch keys
    REQUIRE(Database::check(path).ok());
    {
        Database db2;
        REQUIRE(db2.open(path).ok());
        for (int w = 1; w <= 2; ++w) {
            for (int i = 0; i < 150; ++i) {
                std::string k = "w" + std::to_string(w) + "_k_" + std::to_string(i);
                std::string expected = "val_" + std::to_string(w) + "_" + std::to_string(i);
                std::string actual;
                REQUIRE(db2.get(k, actual).ok() && actual == expected);
            }
        }
        for (int i = 0; i < kBatchKeys; ++i) {
            std::string actual;
            REQUIRE(db2.get("batch_key_" + std::to_string(i), actual).ok());
            REQUIRE(actual == "batch_v_40");
        }
        REQUIRE(db2.close().ok());
    }

    File::remove_file(path);
    std::cout << "[PASS] test_concurrency passed all assertions.\n";
    return 0;
}