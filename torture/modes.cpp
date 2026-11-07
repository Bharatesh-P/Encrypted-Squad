#include "torture/modes.hpp"
#include "kvdb/engine.hpp"
#include "kvdb/platform.hpp"
#include "kvdb/record.hpp"
#include "torture/oracle.hpp"
#include "torture/process_win.hpp"
#include <atomic>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace kvdb::torture {

namespace {

std::string to_hex(const std::string& s) {
    if (s.empty()) {
        return "-";
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto b = static_cast<std::uint8_t>(s[i]);
        out[2 * i]     = kHex[(b >> 4) & 0x0Fu];
        out[2 * i + 1] = kHex[b & 0x0Fu];
    }
    return out;
}

std::string make_random_payload(std::mt19937_64& rng, std::size_t len) {
    static constexpr char kChars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    std::string out(len, 'a');
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = kChars[rng() % (sizeof(kChars) - 1)];
    }
    return out;
}

// Appends a deliberately torn record to the end of `db_path` to force tail repair during `recovery-crash`
void append_torn_tail_for_recovery_crash(const std::string& db_path, std::uint64_t txn_id_hint) {
    File f;
    if (!f.open(db_path, OpenMode::READ_WRITE_CREATE).ok()) {
        return;
    }
    auto sz_res = f.size();
    if (!sz_res.ok() || sz_res.value < kFileHeaderSize) {
        return;
    }
    std::string key = "torn_recovery_key";
    std::string val(4096, 'Z');
    std::vector<std::uint8_t> buf;
    serialize_record(RecordType::PUT, kFlagNone, txn_id_hint, 0, 1, key, val, buf);
    // Write only half the record so recovery must truncate it
    f.write_all_at(sz_res.value, buf.data(), buf.size() / 2);
    f.sync();
    f.close();
}

} // namespace

int run_torture_suite(const TortureConfig& cfg) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Clean start for the database and oracle log
    File::remove_file(cfg.db_path);
    File::remove_file(cfg.db_path + ".tmp");
    const std::string oracle_log_path = cfg.db_path + ".oracle.log";
    Oracle oracle(oracle_log_path);

    std::mt19937_64 master_rng(cfg.seed);
    std::atomic<std::uint64_t> global_op_counter{1};

    std::uint64_t effective_max_bytes = cfg.max_bytes;
    if (effective_max_bytes == 0 && cfg.mode == "disk-full") {
        effective_max_bytes = 64u * 1024u; // 64 KiB cap to exercise disk-full & compaction
    }

    std::cout << "====================================================================\n"
              << "Encrypted Squad Torture Tool\n"
              << "  Mode       : " << cfg.mode << "\n"
              << "  Iterations : " << cfg.iterations << "\n"
              << "  Seed       : " << cfg.seed << "\n"
              << "  Database   : " << cfg.db_path << "\n"
              << "  Oracle Log : " << oracle_log_path << "\n"
              << "====================================================================\n";

    std::uint32_t passed_iterations = 0;

    for (std::uint32_t iter = 1; iter <= cfg.iterations; ++iter) {
        std::uint64_t iter_seed = master_rng();
        std::mt19937_64 iter_rng(iter_seed);

        std::vector<std::string> serve_args{"serve", cfg.db_path};
        if (effective_max_bytes > 0) {
            serve_args.push_back("--max-bytes");
            serve_args.push_back(std::to_string(effective_max_bytes));
        }

        ChildProcess child;
        if (!child.start(cfg.kvdb_exe_path, serve_args)) {
            std::cerr << "[FAIL] Iteration " << iter << ": Failed to start child process: "
                      << cfg.kvdb_exe_path << "\n";
            return 1;
        }

        std::atomic<bool> stop_writers{false};
        std::mutex        pipe_write_mu;

        // Stdout reader thread: continuously reads `ACK <op-id>` and `ERR <op-id> <CODE>` until pipe EOF
        std::thread reader_thread([&]() {
            std::string line;
            while (child.read_stdout_line(line)) {
                std::istringstream iss(line);
                std::string tag, id;
                if (!(iss >> tag >> id)) {
                    continue;
                }
                if (tag == "ACK") {
                    oracle.record_ack(id);
                } else if (tag == "ERR") {
                    std::string code;
                    iss >> code;
                    oracle.record_err(id, code);
                }
            }
        });

        // Spawn at least 2 concurrent writer threads
        const int num_writers = 2;
        std::vector<std::thread> writer_threads;
        writer_threads.reserve(num_writers);

        for (int w = 0; w < num_writers; ++w) {
            std::uint64_t writer_seed = iter_rng() ^ (static_cast<std::uint64_t>(w + 1) * 0x9E3779B97F4A7C15ull);
            writer_threads.emplace_back([&, w, writer_seed]() {
                std::mt19937_64 wrng(writer_seed);

                while (!stop_writers.load(std::memory_order_relaxed)) {
                    std::uint64_t id_num = global_op_counter.fetch_add(1, std::memory_order_relaxed);
                    std::string op_id = "op_" + std::to_string(iter) + "_" + std::to_string(id_num);

                    if (cfg.mode == "large-values") {
                        // Target 1 MB values (up to 1,000,000 bytes)
                        std::size_t val_len = (wrng() % 3 == 0) ? 1000000u : (250000u + (wrng() % 750000u));
                        std::string key = "lv_key_" + std::to_string(wrng() % 16);
                        std::string val = make_random_payload(wrng, val_len);

                        OracleOperation op;
                        op.op_id = op_id;
                        op.kind  = OracleOpKind::SINGLE_PUT;
                        op.key   = key;
                        op.value = val;

                        std::string wire = "PUT " + op_id + " " + to_hex(key) + " " + to_hex(val) + "\n";
                        std::lock_guard<std::mutex> plock(pipe_write_mu);
                        if (stop_writers.load(std::memory_order_relaxed)) break;
                        oracle.record_submitted(std::move(op));
                        if (!child.write_stdin(wire)) break;

                    } else if (cfg.mode == "large-batches") {
                        // Target large batches up to 10,000 operations
                        std::size_t bcount = (w == 0 && (wrng() % 4 == 0))
                                                 ? 10000u
                                                 : (500u + (wrng() % 2000u));
                        OracleOperation op;
                        op.op_id = op_id;
                        op.kind  = OracleOpKind::BATCH;
                        op.batch_ops.reserve(bcount);

                        std::ostringstream wire_ss;
                        wire_ss << "BATCH " << op_id << " " << bcount << "\n";

                        for (std::size_t b = 0; b < bcount; ++b) {
                            std::string k = "lb_k_" + std::to_string(w) + "_" + std::to_string(b);
                            std::string v = "v_" + std::to_string(iter) + "_" + std::to_string(b);
                            op.batch_ops.push_back(BatchOp::Put(k, v));
                            wire_ss << "PUT " << to_hex(k) << " " << to_hex(v) << "\n";
                        }

                        std::string wire = wire_ss.str();
                        std::lock_guard<std::mutex> plock(pipe_write_mu);
                        if (stop_writers.load(std::memory_order_relaxed)) break;
                        oracle.record_submitted(std::move(op));
                        if (!child.write_stdin(wire)) break;

                    } else if (cfg.mode == "compaction") {
                        std::uint64_t roll = wrng() % 10;
                        if (roll < 6) {
                            std::string k = "cmp_k_" + std::to_string(wrng() % 40);
                            std::string v = make_random_payload(wrng, 128 + (wrng() % 1024));
                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::SINGLE_PUT;
                            op.key   = k;
                            op.value = v;
                            std::string wire = "PUT " + op_id + " " + to_hex(k) + " " + to_hex(v) + "\n";
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;
                        } else if (roll < 8) {
                            std::string k = "cmp_k_" + std::to_string(wrng() % 40);
                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::SINGLE_DEL;
                            op.key   = k;
                            std::string wire = "DEL " + op_id + " " + to_hex(k) + "\n";
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;
                        } else {
                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::COMPACT;
                            std::string wire = "COMPACT " + op_id + "\n";
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;
                        }

                    } else {
                        // Default `random`, `recovery-crash`, and `disk-full` traffic:
                        // Mix of single PUTs, single DELs, and multi-op BATCHes
                        std::uint64_t roll = wrng() % 10;
                        if (roll < 5) {
                            // Single PUT (some shared keys, some writer-specific keys)
                            std::string k = "key_" + std::to_string(wrng() % 64);
                            std::size_t vlen = (cfg.mode == "disk-full")
                                                   ? (512 + (wrng() % 2048))
                                                   : (wrng() % 512);
                            std::string v = make_random_payload(wrng, vlen);

                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::SINGLE_PUT;
                            op.key   = k;
                            op.value = v;

                            std::string wire = "PUT " + op_id + " " + to_hex(k) + " " + to_hex(v) + "\n";
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;

                        } else if (roll < 7) {
                            // Single DEL
                            std::string k = "key_" + std::to_string(wrng() % 64);
                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::SINGLE_DEL;
                            op.key   = k;

                            std::string wire = "DEL " + op_id + " " + to_hex(k) + "\n";
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;

                        } else {
                            // Atomic Batch of 4 to 16 operations on batch-dedicated keys + shared keys
                            std::size_t bcount = 4 + (wrng() % 13);
                            OracleOperation op;
                            op.op_id = op_id;
                            op.kind  = OracleOpKind::BATCH;
                            op.batch_ops.reserve(bcount);

                            std::ostringstream wire_ss;
                            wire_ss << "BATCH " << op_id << " " << bcount << "\n";

                            for (std::size_t b = 0; b < bcount; ++b) {
                                std::string bk = "bkey_" + op_id + "_" + std::to_string(b);
                                if (b + 1 == bcount && (wrng() % 3 == 0)) {
                                    // Exercise B5: PUT then DEL same key inside the same batch
                                    std::string first_k = op.batch_ops.front().key;
                                    op.batch_ops.push_back(BatchOp::Del(first_k));
                                    wire_ss << "DEL " << to_hex(first_k) << "\n";
                                } else {
                                    std::string bv = make_random_payload(wrng, 16 + (wrng() % 64));
                                    op.batch_ops.push_back(BatchOp::Put(bk, bv));
                                    wire_ss << "PUT " << to_hex(bk) << " " << to_hex(bv) << "\n";
                                }
                            }

                            std::string wire = wire_ss.str();
                            std::lock_guard<std::mutex> plock(pipe_write_mu);
                            if (stop_writers.load(std::memory_order_relaxed)) break;
                            oracle.record_submitted(std::move(op));
                            if (!child.write_stdin(wire)) break;
                        }
                    }
                }
            });
        }

        // Wait a genuinely random delay in milliseconds, then kill the child process with SIGKILL / TerminateProcess
        std::uint64_t min_ms = 5;
        std::uint64_t max_ms = (cfg.mode == "large-values" || cfg.mode == "large-batches") ? 45 : 25;
        std::uint64_t delay_ms = min_ms + (iter_rng() % (max_ms - min_ms + 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        child.kill_hard();
        stop_writers.store(true, std::memory_order_relaxed);
        child.close_stdin();

        for (auto& wt : writer_threads) {
            if (wt.joinable()) {
                wt.join();
            }
        }

        child.wait_for_exit();
        if (reader_thread.joinable()) {
            reader_thread.join();
        }

        // If mode is `recovery-crash`, inject a torn tail and kill `kvdb open` during recovery
        if (cfg.mode == "recovery-crash") {
            append_torn_tail_for_recovery_crash(cfg.db_path, 9999999ull);
            ChildProcess rec_child;
            if (rec_child.start(cfg.kvdb_exe_path, {"open", cfg.db_path})) {
                std::this_thread::sleep_for(std::chrono::milliseconds(iter_rng() % 6));
                rec_child.kill_hard();
                rec_child.wait_for_exit();
            }
        }

        // Step 5: Restart the database with open, run check, then dump
        Database recovered_db;
        OpenOptions open_opts{};
        if (effective_max_bytes > 0) {
            open_opts.max_bytes = effective_max_bytes;
        }
        Status open_st = recovered_db.open(cfg.db_path, open_opts);
        if (!open_st.ok()) {
            std::cerr << "[FAIL] Iteration " << iter << " (seed=" << cfg.seed << "): "
                      << "open() failed after kill: " << open_st.to_string() << "\n";
            return 1;
        }
        recovered_db.close();

        Status check_st = Database::check(cfg.db_path);
        if (!check_st.ok()) {
            std::cerr << "[FAIL] Iteration " << iter << " (seed=" << cfg.seed << "): "
                      << "check() failed after open() recovery: " << check_st.to_string() << "\n";
            return 1;
        }

        Status reopen_st = recovered_db.open(cfg.db_path, open_opts);
        if (!reopen_st.ok()) {
            std::cerr << "[FAIL] Iteration " << iter << " (seed=" << cfg.seed << "): "
                      << "reopen for dump() failed: " << reopen_st.to_string() << "\n";
            return 1;
        }

        std::vector<std::pair<std::string, std::string>> dump_items;
        Status dump_st = recovered_db.dump(dump_items);
        recovered_db.close();
        if (!dump_st.ok()) {
            std::cerr << "[FAIL] Iteration " << iter << " (seed=" << cfg.seed << "): "
                      << "dump() failed: " << dump_st.to_string() << "\n";
            return 1;
        }

        // Step 6: Compare dump against the independent oracle
        VerificationReport report = oracle.verify_and_advance(dump_items);
        if (!report.passed) {
            std::cerr << "\n====================================================================\n"
                      << "[INVARIANT VIOLATION] Iteration " << iter << " / " << cfg.iterations << "\n"
                      << "  Seed            : " << cfg.seed << "\n"
                      << "  Mode            : " << cfg.mode << "\n"
                      << "  Reason          : " << report.failure_reason << "\n"
                      << "  Key             : " << report.bad_key << "\n"
                      << "  Expected        : " << report.expected_desc << "\n"
                      << "  Found           : " << report.found_value << "\n"
                      << "  Operation ID(s) : " << report.involved_op_ids << "\n"
                      << "====================================================================\n";
            return 1;
        }

        ++passed_iterations;
        std::cout << "[PASS] iter=" << std::setw(3) << iter << "/" << cfg.iterations
                  << " mode=" << cfg.mode
                  << " delay_ms=" << std::setw(2) << delay_ms
                  << " live_keys=" << std::setw(4) << dump_items.size()
                  << " total_acked=" << oracle.total_acked_count()
                  << " recovery_ms=" << std::fixed << std::setprecision(2) << recovered_db.last_recovery_ms()
                  << "\n" << std::flush;
    }

    std::cout << "====================================================================\n"
              << "FINAL SUMMARY: PASS=" << passed_iterations << " FAIL=0"
              << " (seed=" << cfg.seed << ", mode=" << cfg.mode
              << ", total_acked=" << oracle.total_acked_count() << ")\n"
              << "====================================================================\n";
    return 0;
}

} // namespace kvdb::torture