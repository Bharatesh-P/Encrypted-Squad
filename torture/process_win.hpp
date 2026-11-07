#ifndef KVDB_TORTURE_PROCESS_WIN_HPP
#define KVDB_TORTURE_PROCESS_WIN_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace kvdb::torture {

class ChildProcess {
public:
    ChildProcess() noexcept;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Spawns `exe_path` with `args` and pipes connected to stdin and stdout
    bool start(const std::string& exe_path, const std::vector<std::string>& args);

    // Writes raw bytes to child's stdin pipe; returns false if pipe is broken/closed
    bool write_stdin(const std::string& data);

    // Closes child's stdin pipe
    void close_stdin();

    // Reads one line from child's stdout pipe; returns false on EOF
    bool read_stdout_line(std::string& out_line);

    // Immediately kills the child process with TerminateProcess (Windows) / SIGKILL (POSIX)
    void kill_hard();

    // Waits for the child process to exit and returns exit code
    int wait_for_exit();

    bool is_running() const;

private:
#ifdef _WIN32
    void* process_handle_;
    void* stdin_write_;
    void* stdout_read_;
#else
    int   pid_;
    int   stdin_write_fd_;
    int   stdout_read_fd_;
#endif
    std::string stdout_buffer_;
};

} // namespace kvdb::torture

#endif // KVDB_TORTURE_PROCESS_WIN_HPP