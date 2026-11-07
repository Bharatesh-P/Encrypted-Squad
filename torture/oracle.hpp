#ifndef KVDB_TORTURE_ORACLE_HPP
#define KVDB_TORTURE_ORACLE_HPP

#include "kvdb/engine.hpp"
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kvdb::torture {

enum class OracleOpKind {
    SINGLE_PUT,
    SINGLE_DEL,
    BATCH,
    COMPACT
};

struct OracleOperation {
    std::string          op_id;
    OracleOpKind         kind         = OracleOpKind::SINGLE_PUT;
    std::string          key;
    std::string          value;
    std::vector<BatchOp> batch_ops;
    bool                 acked        = false;
    bool                 errored      = false;
    std::string          error_code;
    std::uint64_t        ack_order    = 0;
};

struct VerificationReport {
    bool        passed = true;
    std::string failure_reason;
    std::string bad_key;
    std::string expected_desc;
    std::string found_value;
    std::string involved_op_ids;
};

class Oracle {
public:
    explicit Oracle(const std::string& log_path);
    ~Oracle();

    Oracle(const Oracle&) = delete;
    Oracle& operator=(const Oracle&) = delete;

    // Called by writer threads BEFORE sending bytes over the pipe to `kvdb serve`
    void record_submitted(OracleOperation op);

    // Called by the stdout reader thread when `ACK <op-id>` is received
    void record_ack(const std::string& op_id);

    // Called by the stdout reader thread when `ERR <op-id> <CODE>` is received
    void record_err(const std::string& op_id, const std::string& err_code);

    // Verifies the recovered database dump after a kill/restart cycle, and synchronizes
    // the oracle's baseline state to the verified recovered state for the next iteration.
    VerificationReport verify_and_advance(
        const std::vector<std::pair<std::string, std::string>>& recovered_dump);

    std::size_t total_acked_count() const;
    std::size_t total_err_count() const;

private:
    static void apply_op_to_map(
        const OracleOperation& op,
        std::map<std::string, std::string>& state);

    mutable std::mutex                                  mu_;
    std::ofstream                                       log_file_;
    std::map<std::string, std::string>                  confirmed_state_;
    std::unordered_map<std::string, OracleOperation>    current_cycle_ops_;
    std::vector<std::string>                            ack_sequence_;
    std::uint64_t                                       next_ack_order_ = 1;
    std::size_t                                         total_acked_    = 0;
    std::size_t                                         total_errored_  = 0;
};

} // namespace kvdb::torture

#endif // KVDB_TORTURE_ORACLE_HPP