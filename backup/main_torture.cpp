#include "torture/modes.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout <<
        "Usage: torture [options]\n"
        "  --iterations N   number of kill/recover cycles (default 500)\n"
        "  --seed N         RNG seed (default 12345)\n"
        "  --mode M         random | large-values | large-batches | compaction |\n"
        "                   recovery-crash | disk-full (default random)\n"
        "  --db PATH        database file (default torture_db.data)\n"
        "  --kvdb PATH      path to kvdb executable (default: next to torture)\n"
        "  --max-bytes N    file size cap (0 = mode default)\n";
}

std::string default_kvdb_path(const char* argv0) {
    std::string self = argv0 ? argv0 : "";
    auto pos = self.find_last_of("/\\");
    std::string dir = (pos == std::string::npos) ? "." : self.substr(0, pos);
#ifdef _WIN32
    return dir + "\\kvdb.exe";
#else
    return dir + "/kvdb";
#endif
}

} // namespace

int main(int argc, char** argv) {
    kvdb::torture::TortureConfig cfg;
    cfg.kvdb_exe_path = default_kvdb_path(argc > 0 ? argv[0] : nullptr);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--iterations")     cfg.iterations = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--seed")      cfg.seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--mode")      cfg.mode = next();
        else if (a == "--db")        cfg.db_path = next();
        else if (a == "--kvdb")      cfg.kvdb_exe_path = next();
        else if (a == "--max-bytes") cfg.max_bytes = std::strtoull(next(), nullptr, 10);
        else if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else { std::cerr << "Unknown option: " << a << "\n"; print_usage(); return 2; }
    }
    return kvdb::torture::run_torture_suite(cfg);
}
