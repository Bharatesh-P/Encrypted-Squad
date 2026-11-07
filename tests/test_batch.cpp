#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include <iostream>
#include <string>
#include <vector>

#define REQUIRE(cond, msg)                                                        \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << msg \
                      << "\n";                                                    \
            return 1;                                                             \
        }                                                                         \
    } while (0)

int main() {
    using namespace kvdb;
    const std::string db_path = "test_batch_db.data";
    File::remove_file(db_path);

    {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Open failed");

        // 1. 1-operation batch
        REQUIRE(db.batch({BatchOp::Put("single_b", "val_b")}).ok(), "1-op batch failed");
        std::string val;
        REQUIRE(db.get("single_b", val).ok() && val == "val_b", "1-op batch value mismatch");

        // 2. Scenario B5: Batch that PUTs key X and DELETEs key X in the same batch,
        // and DELETEs key Y then PUTs key Y in the same batch.
        std::vector<BatchOp> b5_ops = {
            BatchOp::Put("keyX", "first_val"),
            BatchOp::Put("keyX", "second_val"),
            BatchOp::Del("keyX"),
            BatchOp::Put("keyY", "before_del"),
            BatchOp::Del("keyY"),
            BatchOp::Put("keyY", "recreated_after_del")
        };
        REQUIRE(db.batch(b5_ops).ok(), "B5 batch failed");
        REQUIRE(db.get("keyX", val).is_not_found(), "keyX should be absent after PUT-then-DEL in batch");
        REQUIRE(db.get("keyY", val).ok() && val == "recreated_after_del", "keyY ordering failed");

        // 3. Scenario B6: Maximum 10,000-operation atomic batch
        std::vector<BatchOp> large_ops;
        large_ops.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            large_ops.push_back(BatchOp::Put("b10k_" + std::to_string(i), "v_" + std::to_string(i)));
        }
        REQUIRE(db.batch(large_ops).ok(), "10,000-op batch failed");
        REQUIRE(db.size() == 10002, "Expected 10,002 live keys after 10k batch");
        REQUIRE(db.close().ok(), "Close failed");
    }

    // 4. Reopen and verify all 10,002 keys survive recovery and B5 ordering holds after restart
    {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Reopen after 10k batch failed");
        REQUIRE(db.size() == 10002, "Key count mismatch after recovery");

        std::string val;
        REQUIRE(db.get("keyX", val).is_not_found(), "keyX reappeared after recovery");
        REQUIRE(db.get("keyY", val).ok() && val == "recreated_after_del", "keyY wrong after recovery");
        REQUIRE(db.get("b10k_9999", val).ok() && val == "v_9999", "b10k_9999 wrong after recovery");
        REQUIRE(db.close().ok(), "Close failed");
    }

    File::remove_file(db_path);
    std::cout << "[PASS] test_batch passed all assertions.\n";
    return 0;
}