#ifndef KVDB_TORTURE_MODES_HPP
#define KVDB_TORTURE_MODES_HPP

#include <cstdint>
#include <string>

namespace kvdb::torture {

struct TortureConfig {
    std::string   kvdb_exe_path;
    std::string   db_path       = "torture_db.data";
    std::string   mode          = "random";
    std::uint32_t iterations    = 500;
    std::uint64_t seed          = 12345;
    std::uint64_t max_bytes     = 0; // 0 = use mode default
};

// Runs the complete configured torture test loop.
// Returns 0 if all iterations pass with zero invariant violations, or 1 on first failure.
int run_torture_suite(const TortureConfig& cfg);

} // namespace kvdb::torture

#endif // KVDB_TORTURE_MODES_HPP