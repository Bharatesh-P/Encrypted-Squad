#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include <iostream>
#include <string>
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
    const std::string path = "test_disk_full_db.data";
    File::remove_file(path);
    File::remove_file(path + ".tmp");

    // Configure a strict 4,096-byte cap
    OpenOptions opts{};
    opts.max_bytes = 4096;

    Database db;
    REQUIRE(db.open(path, opts).ok());

    // 1. Write 10 keys (each record is 32 + 4 + 200 = 236 bytes -> 2,360 bytes + 12 header = 2,372 bytes)
    for (int i = 0; i < 10; ++i) {
        std::string k = "k00" + std::to_string(i);
        REQUIRE(db.put(k, std::string(200, 'X')).ok());
    }

    // 2. Scenario E1: Single PUT that exceeds 4,096 bytes must fail cleanly with DISK_FULL
    Status s_put = db.put("k_huge", std::string(3000, 'Y'));
    REQUIRE(s_put.is_disk_full());

    // Existing keys must remain readable
    std::string val;
    REQUIRE(db.get("k000", val).ok() && val == std::string(200, 'X'));
    REQUIRE(db.get("k_huge", val).is_not_found());

    // 3. Scenario E2: Batch that exceeds cap halfway through must fail with DISK_FULL and apply ZERO ops
    std::vector<BatchOp> bops;
    for (int i = 0; i < 15; ++i) {
        bops.push_back(BatchOp::Put("b_" + std::to_string(i), std::string(200, 'Z')));
    }
    Status s_batch = db.batch(bops);
    REQUIRE(s_batch.is_disk_full());
    REQUIRE(db.get("b_0", val).is_not_found());

    // 4. Scenario E5: Delete 8 keys, compact (or write to trigger auto-compaction), and write new keys
    for (int i = 2; i < 10; ++i) {
        std::string k = "k00" + std::to_string(i);
        REQUIRE(db.del(k).ok());
    }
    REQUIRE(db.compact().ok());
    REQUIRE(db.file_size() < 600);

    // Writes now succeed again within the 4,096-byte cap!
    for (int i = 0; i < 5; ++i) {
        REQUIRE(db.put("new_" + std::to_string(i), std::string(200, 'W')).ok());
    }
    REQUIRE(db.close().ok());

    // 5. Scenario E3: Reopen and verify all data is intact
    REQUIRE(Database::check(path).ok());
    {
        Database db2;
        REQUIRE(db2.open(path, opts).ok());
        REQUIRE(db2.size() == 7); // k000, k001 + new_0..new_4
        REQUIRE(db2.get("k000", val).ok() && val == std::string(200, 'X'));
        REQUIRE(db2.get("new_4", val).ok() && val == std::string(200, 'W'));
        REQUIRE(db2.close().ok());
    }

    File::remove_file(path);
    std::cout << "[PASS] test_disk_full passed all assertions.\n";
    return 0;
}