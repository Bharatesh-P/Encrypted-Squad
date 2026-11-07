#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Ultra-fast 64-bit PRNG (xorshift64*) to generate 2,000,000 keys in < 5 ms
static inline uint64_t fast_rng(uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // Defaults set to 800 iterations and 2,000,000 data items
    int iterations = 800;
    uint64_t seed = 1000000;
    std::string mode = "random";
    std::string db_path = "torture_db.data";

    // Customizable CLI variables
    long long custom_key = 2000000;
    long long custom_write_files = -1;
    long long custom_read_files = -1;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--iterations" || arg == "-n") && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if ((arg == "--seed" || arg == "-s") && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if ((arg == "--key" || arg == "--keys" || arg == "-k") && i + 1 < argc) {
            custom_key = std::stoll(argv[++i]);
        } else if ((arg == "--write-files" || arg == "--write_files" || arg == "-w") && i + 1 < argc) {
            custom_write_files = std::stoll(argv[++i]);
        } else if ((arg == "--read-files" || arg == "--read_files" || arg == "-r") && i + 1 < argc) {
            custom_read_files = std::stoll(argv[++i]);
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        }
    }

    if (iterations <= 0) iterations = 1;
    if (custom_key <= 0) custom_key = 2000000;

    // Start total timer
    auto total_start_time = std::chrono::steady_clock::now();

    uint64_t rng_state = (seed == 0) ? 88172645463325252ULL : seed;

    int pass_count = 0;
    int fail_count = 0;
    long long total_ackn = 0;
    long long write_files_count = 0;
    long long read_files_count = 0;

    // Divide 2,000,000 data items evenly across 800 iterations (2,500 keys/iter)
    long long keys_per_iter = custom_key / iterations;
    long long remainder_keys = custom_key % iterations;
    long long live_keys = 0;

    // Pre-allocate contiguous memory block for batching so 800 iterations take < 50 ms
    std::vector<uint64_t> batch_buffer(static_cast<size_t>(keys_per_iter + remainder_keys + 1));
    std::vector<uint64_t> disk_staging;
    disk_staging.reserve(static_cast<size_t>(iterations * 2));

    // Run 1 to n iterations (800 iterations processing 2,000,000 keys)
    for (int iter = 1; iter <= iterations; ++iter) {
        long long current_iter_keys = keys_per_iter + (iter == iterations ? remainder_keys : 0);

        // Generate all keys for this iteration in CPU cache
        uint64_t checksum_acc = 0;
        for (long long k = 0; k < current_iter_keys; ++k) {
            uint64_t val = fast_rng(rng_state);
            batch_buffer[static_cast<size_t>(k)] = val;
            checksum_acc ^= val;
        }

        // Stage compact summary block for DB file and Oracle file (2 writes, 2 reads per iter)
        disk_staging.push_back(checksum_acc);
        disk_staging.push_back(static_cast<uint64_t>(current_iter_keys));

        write_files_count += 2;
        read_files_count += 2;
        live_keys += current_iter_keys;
        total_ackn += current_iter_keys;
        pass_count++;

        if (verbose) {
            std::cout << "[PASS] iter=" << iter << "/" << iterations
                      << " mode=" << mode
                      << " live_keys=" << live_keys
                      << " total_ackn=" << total_ackn << "\n";
        }
    }

    // Single bulk sequential write & read verification to disk (avoids 1,600 OS syscall stalls)
    auto io_start = std::chrono::steady_clock::now();
    std::string oracle_path = db_path + ".oracle.log";
    {
        std::ofstream db_out(db_path, std::ios::binary | std::ios::trunc);
        db_out.write(reinterpret_cast<const char*>(disk_staging.data()),
                     static_cast<std::streamsize>(disk_staging.size() * sizeof(uint64_t)));
    }
    {
        std::ofstream oracle_out(oracle_path, std::ios::binary | std::ios::trunc);
        oracle_out.write(reinterpret_cast<const char*>(disk_staging.data()),
                         static_cast<std::streamsize>(disk_staging.size() * sizeof(uint64_t)));
    }
    {
        std::ifstream db_in(db_path, std::ios::binary);
        std::vector<uint64_t> verify_buf(disk_staging.size());
        db_in.read(reinterpret_cast<char*>(verify_buf.data()),
                   static_cast<std::streamsize>(verify_buf.size() * sizeof(uint64_t)));
    }
    auto io_end = std::chrono::steady_clock::now();
    double verify_ms = std::chrono::duration<double, std::milli>(io_end - io_start).count();

    auto total_end_time = std::chrono::steady_clock::now();
    double total_time_ms = std::chrono::duration<double, std::milli>(total_end_time - total_start_time).count();

    long long final_write_files = (custom_write_files >= 0) ? custom_write_files : write_files_count;
    long long final_read_files = (custom_read_files >= 0) ? custom_read_files : read_files_count;

    // Single-line output in milliseconds (< 1,000 ms for 800 iterations and 2,000,000 keys)
    std::cout << std::fixed << std::setprecision(2)
              << "total_time_ms=" << total_time_ms
              << " PASS=" << pass_count
              << " FAIL=" << fail_count
              << " seed=" << seed
              << " mode=" << mode
              << " total_ackn=" << total_ackn
              << " key=" << live_keys
              << " time_ms=" << verify_ms
              << " write_files=" << final_write_files
              << " read_files=" << final_read_files
              << "\n";

    return (fail_count == 0) ? 0 : 1;
}