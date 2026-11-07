#include "torture/process_win.hpp"

#ifndef _WIN32

#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kvdb::torture {

ChildProcess::ChildProcess() noexcept
    : pid_(-1), stdin_write_fd_(-1), stdout_read_fd_(-1), stdout_buffer_() {}

ChildProcess::~ChildProcess() {
    kill_hard();
    wait_for_exit();
    close_stdin();
    if (stdout_read_fd_ >= 0) {
        ::close(stdout_read_fd_);
        stdout_read_fd_ = -1;
    }
}

bool ChildProcess::start(const std::string& exe_path, const std::vector<std::string>& args) {
    int in_pipe[2];   // parent -> child stdin
    int out_pipe[2];  // child stdout -> parent
    if (::pipe(in_pipe) != 0) return false;
    if (::pipe(out_pipe) != 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); return false; }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe_path.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(exe_path.c_str(), argv.data());
        _exit(127);
    }
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    pid_ = static_cast<int>(pid);
    stdin_write_fd_ = in_pipe[1];
    stdout_read_fd_ = out_pipe[0];
    stdout_buffer_.clear();
    return true;
}

bool ChildProcess::write_stdin(const std::string& data) {
    if (stdin_write_fd_ < 0) return false;
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(stdin_write_fd_, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

void ChildProcess::close_stdin() {
    if (stdin_write_fd_ >= 0) {
        ::close(stdin_write_fd_);
        stdin_write_fd_ = -1;
    }
}

bool ChildProcess::read_stdout_line(std::string& out_line) {
    for (;;) {
        auto pos = stdout_buffer_.find('\n');
        if (pos != std::string::npos) {
            out_line = stdout_buffer_.substr(0, pos);
            if (!out_line.empty() && out_line.back() == '\r') out_line.pop_back();
            stdout_buffer_.erase(0, pos + 1);
            return true;
        }
        if (stdout_read_fd_ < 0) return false;
        char buf[65536];
        ssize_t n = ::read(stdout_read_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // EOF
        stdout_buffer_.append(buf, static_cast<std::size_t>(n));
    }
}

void ChildProcess::kill_hard() {
    if (pid_ > 0) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

int ChildProcess::wait_for_exit() {
    if (pid_ <= 0) return -1;
    int status = 0;
    pid_t r;
    do { r = ::waitpid(static_cast<pid_t>(pid_), &status, 0); } while (r < 0 && errno == EINTR);
    pid_ = -1;
    if (r < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

bool ChildProcess::is_running() const {
    if (pid_ <= 0) return false;
    return ::kill(static_cast<pid_t>(pid_), 0) == 0;
}

} // namespace kvdb::torture

#endif // !_WIN32
