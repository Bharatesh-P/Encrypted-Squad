#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include "kvdb/record.hpp"
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
    const std::string db_path = "test_recovery_db.data";
    File::remove_file(db_path);
    File::remove_file(db_path + ".tmp");

    // 1. Write two valid records and close cleanly
    {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Initial open failed");
        REQUIRE(db.put("k1", "v1_initial").ok(), "put k1 failed");
        REQUIRE(db.put("k2", "v2_initial").ok(), "put k2 failed");
        REQUIRE(db.close().ok(), "close failed");
    }

    // 2. Scenario F4: Manually truncate 1 byte off the end of the file (simulates a torn tail)
    {
        File raw;
        REQUIRE(raw.open(db_path, OpenMode::READ_WRITE_CREATE).ok(), "Raw open failed");
        auto sz = raw.size();
        REQUIRE(sz.ok() && sz.value > kFileHeaderSize, "Invalid file size");
        REQUIRE(raw.truncate(sz.value - 1).ok(), "Manual 1-byte truncate failed");
        REQUIRE(raw.close().ok(), "Raw close failed");
    }

    // Reopen: k1 must survive intact, torn k2 tail must be truncated cleanly, check() must say OK
    {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Open after 1-byte tail truncation failed");
        std::string val;
        REQUIRE(db.get("k1", val).ok() && val == "v1_initial", "k1 lost after tail truncation");
        REQUIRE(db.get("k2", val).is_not_found(), "Torn k2 record should have been rolled back");
        REQUIRE(db.close().ok(), "Close failed");
        REQUIRE(Database::check(db_path).ok(), "Database::check failed after tail repair");
    }

    // 3. Scenario B1/B2: Append 3 batch member records WITHOUT a COMMIT marker at EOF
    {
        File raw;
        REQUIRE(raw.open(db_path, OpenMode::READ_WRITE_CREATE).ok(), "Raw open failed");
        auto sz = raw.size();
        REQUIRE(sz.ok(), "Size query failed");
        std::uint64_t offset = sz.value;

        for (std::uint32_t i = 0; i < 3; ++i) {
            std::vector<std::uint8_t> buf;
            serialize_record(
                RecordType::PUT,
                kFlagBatchMember,
                2, // next txn_id after k1 (txn_id=1)
                i,
                5, // declares 5 ops, only 3 written and no COMMIT marker
                "uncommitted_k" + std::to_string(i),
                "uncommitted_val",
                buf);
            REQUIRE(raw.write_all_at(offset, buf.data(), buf.size()).ok(), "Raw batch member write failed");
            offset += buf.size();
        }
        REQUIRE(raw.sync().ok(), "Raw sync failed");
        REQUIRE(raw.close().ok(), "Raw close failed");
    }

    // Reopen: all 3 uncommitted batch members must be rolled back to batch_start
    {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Open after uncommitted batch failed");
        REQUIRE(db.size() == 1, "Uncommitted batch members leaked into index");
        std::string val;
        REQUIRE(db.get("uncommitted_k0", val).is_not_found(), "uncommitted_k0 should not exist");
        REQUIRE(db.get("k1", val).ok() && val == "v1_initial", "k1 must still be intact");
        REQUIRE(db.close().ok(), "Close failed");
        REQUIRE(Database::check(db_path).ok(), "Database::check failed after batch rollback");
    }

    // 4. Scenario F2: Repeated opens are idempotent
    for (int i = 0; i < 5; ++i) {
        Database db;
        REQUIRE(db.open(db_path).ok(), "Repeated open failed");
        REQUIRE(db.size() == 1, "Key count changed across idempotent opens");
        REQUIRE(db.close().ok(), "Close failed");
    }

    File::remove_file(db_path);
    std::cout << "[PASS] test_recovery passed all assertions.\n";
    return 0;
}
