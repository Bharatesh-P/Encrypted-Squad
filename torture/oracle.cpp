#include "torture/oracle.hpp"
#include <algorithm>
#include <sstream>

namespace kvdb::torture {

Oracle::Oracle(const std::string& log_path) {
    log_file_.open(log_path, std::ios::out | std::ios::trunc);
}

Oracle::~Oracle() {
    if (log_file_.is_open()) {
        log_file_.flush();
        log_file_.close();
    }
}

void Oracle::record_submitted(OracleOperation op) {
    std::lock_guard<std::mutex> lock(mu_);
    current_cycle_ops_[op.op_id] = std::move(op);
}

void Oracle::record_ack(const std::string& op_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = current_cycle_ops_.find(op_id);
    if (it == current_cycle_ops_.end()) {
        return;
    }
    if (!it->second.acked) {
        it->second.acked     = true;
        it->second.ack_order = next_ack_order_++;
        ack_sequence_.push_back(op_id);
        ++total_acked_;

        if (log_file_.is_open()) {
            log_file_ << "ACK " << op_id << "\n";
            log_file_.flush();
        }
    }
}

void Oracle::record_err(const std::string& op_id, const std::string& err_code) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = current_cycle_ops_.find(op_id);
    if (it != current_cycle_ops_.end()) {
        it->second.errored    = true;
        it->second.error_code = err_code;
        ++total_errored_;
        if (log_file_.is_open()) {
            log_file_ << "ERR " << op_id << " " << err_code << "\n";
            log_file_.flush();
        }
    }
}

void Oracle::apply_op_to_map(
    const OracleOperation& op,
    std::map<std::string, std::string>& state) {

    switch (op.kind) {
        case OracleOpKind::SINGLE_PUT:
            state[op.key] = op.value;
            break;
        case OracleOpKind::SINGLE_DEL:
            state.erase(op.key);
            break;
        case OracleOpKind::BATCH:
            for (const auto& bop : op.batch_ops) {
                if (bop.type == BatchOpType::PUT) {
                    state[bop.key] = bop.value;
                } else {
                    state.erase(bop.key);
                }
            }
            break;
        case OracleOpKind::COMPACT:
            break;
    }
}

VerificationReport Oracle::verify_and_advance(
    const std::vector<std::pair<std::string, std::string>>& recovered_dump) {

    std::lock_guard<std::mutex> lock(mu_);
    VerificationReport report;

    std::map<std::string, std::string> actual_map;
    for (const auto& kv : recovered_dump) {
        actual_map[kv.first] = kv.second;
    }

    // 1. Replay all ACKed operations in their exact ACK order onto confirmed_state_
    std::map<std::string, std::string> base_after_acks = confirmed_state_;
    std::map<std::string, std::string> last_ack_op_for_key;

    for (const std::string& id : ack_sequence_) {
        auto it = current_cycle_ops_.find(id);
        if (it == current_cycle_ops_.end()) {
            continue;
        }
        const OracleOperation& op = it->second;
        apply_op_to_map(op, base_after_acks);

        if (op.kind == OracleOpKind::SINGLE_PUT || op.kind == OracleOpKind::SINGLE_DEL) {
            last_ack_op_for_key[op.key] = op.op_id;
        } else if (op.kind == OracleOpKind::BATCH) {
            for (const auto& bop : op.batch_ops) {
                last_ack_op_for_key[bop.key] = op.op_id;
            }
        }
    }

    // 2. Collect in-flight (unacknowledged, non-errored) operations
    // An unacknowledged operation may be wholly present or wholly absent after a crash.
    std::vector<const OracleOperation*> inflight_ops;
    for (const auto& kv : current_cycle_ops_) {
        if (!kv.second.acked && !kv.second.errored) {
            inflight_ops.push_back(&kv.second);
        }
    }

    // Build per-key allowed outcomes from in-flight operations
    // Key -> set of allowed optional values (nullptr represented via a separate bool for deletion)
    std::map<std::string, std::set<std::string>> inflight_candidate_values;
    std::set<std::string>                        inflight_can_delete;
    std::map<std::string, std::vector<std::string>> inflight_op_ids_for_key;

    for (const OracleOperation* op : inflight_ops) {
        if (op->kind == OracleOpKind::SINGLE_PUT) {
            inflight_candidate_values[op->key].insert(op->value);
            inflight_op_ids_for_key[op->key].push_back(op->op_id);
        } else if (op->kind == OracleOpKind::SINGLE_DEL) {
            inflight_can_delete.insert(op->key);
            inflight_op_ids_for_key[op->key].push_back(op->op_id);
        } else if (op->kind == OracleOpKind::BATCH) {
            // Compute net effect of this batch on every key it touches
            std::map<std::string, std::pair<bool, std::string>> net_batch;
            for (const auto& bop : op->batch_ops) {
                if (bop.type == BatchOpType::PUT) {
                    net_batch[bop.key] = {true, bop.value};
                } else {
                    net_batch[bop.key] = {false, std::string()};
                }
            }
            for (const auto& nb : net_batch) {
                if (nb.second.first) {
                    inflight_candidate_values[nb.first].insert(nb.second.second);
                } else {
                    inflight_can_delete.insert(nb.first);
                }
                inflight_op_ids_for_key[nb.first].push_back(op->op_id);
            }
        }
    }

    // 3. Verify every ACKed batch is completely reflected (unless overwritten by a later ACK or in-flight op)
    // and verify every in-flight batch is strictly ALL-OR-NOTHING on keys that have no conflicting writers.
    for (const auto& kv : current_cycle_ops_) {
        const OracleOperation& op = kv.second;
        if (op.kind != OracleOpKind::BATCH || op.errored) {
            continue;
        }
        // Compute net final state per key within this batch (Scenario B5)
        std::map<std::string, std::pair<bool, std::string>> net_batch;
        for (const auto& bop : op.batch_ops) {
            if (bop.type == BatchOpType::PUT) {
                net_batch[bop.key] = {true, bop.value};
            } else {
                net_batch[bop.key] = {false, std::string()};
            }
        }

        if (!op.acked) {
            // Check all-or-nothing on keys uniquely touched by this in-flight batch
            std::size_t matched_applied = 0;
            std::size_t matched_absent  = 0;
            std::string sample_applied_key;
            std::string sample_absent_key;

            for (const auto& nb : net_batch) {
                const std::string& k = nb.first;
                if (inflight_op_ids_for_key[k].size() != 1) {
                    continue; // Skip keys concurrently touched by another in-flight op
                }
                bool batch_puts = nb.second.first;
                const std::string& batch_val = nb.second.second;

                auto base_it = base_after_acks.find(k);
                bool in_base = (base_it != base_after_acks.end());
                const std::string base_val = in_base ? base_it->second : "";

                // Only unambiguous if the batch actually changes the key's state relative to base_after_acks
                if (batch_puts && in_base && base_val == batch_val) {
                    continue;
                }
                if (!batch_puts && !in_base) {
                    continue;
                }

                auto act_it = actual_map.find(k);
                bool in_act = (act_it != actual_map.end());

                bool is_batch_state = batch_puts ? (in_act && act_it->second == batch_val) : (!in_act);
                bool is_base_state  = in_base ? (in_act && act_it->second == base_val) : (!in_act);

                if (is_batch_state) {
                    ++matched_applied;
                    if (sample_applied_key.empty()) sample_applied_key = k;
                } else if (is_base_state) {
                    ++matched_absent;
                    if (sample_absent_key.empty()) sample_absent_key = k;
                } else {
                    report.passed          = false;
                    report.failure_reason  = "Corrupted/unknown value on batch key";
                    report.bad_key         = k;
                    report.expected_desc   = batch_puts ? batch_val : "<DELETED>";
                    report.found_value     = in_act ? act_it->second : "<NOTFOUND>";
                    report.involved_op_ids = op.op_id;
                    return report;
                }
            }

            if (matched_applied > 0 && matched_absent > 0) {
                report.passed          = false;
                report.failure_reason  = "Partial batch visibility violation (Invariant Rule 3)";
                report.bad_key         = sample_applied_key + " vs " + sample_absent_key;
                report.expected_desc   = "All " + std::to_string(op.batch_ops.size()) + " batch ops applied OR 0 applied";
                report.found_value     = std::to_string(matched_applied) + " applied, " + std::to_string(matched_absent) + " absent";
                report.involved_op_ids = op.op_id;
                return report;
            }
        }
    }

    // 4. Verify every key in base_after_acks and actual_map
    std::set<std::string> all_keys;
    for (const auto& kv : base_after_acks) all_keys.insert(kv.first);
    for (const auto& kv : actual_map)      all_keys.insert(kv.first);

    for (const std::string& k : all_keys) {
        auto base_it = base_after_acks.find(k);
        auto act_it  = actual_map.find(k);
        bool in_base = (base_it != base_after_acks.end());
        bool in_act  = (act_it != actual_map.end());

        if (in_base && in_act && base_it->second == act_it->second) {
            continue; // Exact match with ACKed state
        }
        if (!in_base && !in_act) {
            continue;
        }

        // State differs from base_after_acks; check if an in-flight operation explains it
        if (!in_act) {
            // Key is absent in actual_map, but was present in base_after_acks
            if (inflight_can_delete.count(k) > 0) {
                continue; // Explained by an in-flight delete
            }
            report.passed          = false;
            report.failure_reason  = "Acknowledged key lost after crash (Invariant Rule 1)";
            report.bad_key         = k;
            report.expected_desc   = base_it->second;
            report.found_value     = "<NOTFOUND>";
            report.involved_op_ids = last_ack_op_for_key[k];
            return report;
        } else {
            // Key is present in actual_map with value != base_after_acks
            const std::string& found_val = act_it->second;
            auto cand_it = inflight_candidate_values.find(k);
            if (cand_it != inflight_candidate_values.end() &&
                cand_it->second.count(found_val) > 0) {
                continue; // Explained by an in-flight write that completed fsync before kill
            }

            report.passed         = false;
            report.failure_reason = in_base
                                        ? "Key holds wrong/stale/corrupted value"
                                        : "Deleted or unknown key reappeared (Invariant Rule 1/4)";
            report.bad_key        = k;
            report.expected_desc  = in_base ? base_it->second : "<NOTFOUND>";
            report.found_value    = found_val;
            std::string ids       = last_ack_op_for_key[k];
            for (const auto& inf_id : inflight_op_ids_for_key[k]) {
                if (!ids.empty()) ids += ", ";
                ids += inf_id;
            }
            report.involved_op_ids = ids.empty() ? "none" : ids;
            return report;
        }
    }

    // Advance confirmed_state_ to the verified recovered state for the next iteration
    confirmed_state_ = std::move(actual_map);
    current_cycle_ops_.clear();
    ack_sequence_.clear();
    return report;
}

std::size_t Oracle::total_acked_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_acked_;
}

std::size_t Oracle::total_err_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_errored_;
}

} // namespace kvdb::torture