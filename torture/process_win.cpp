#include "torture/process_win.hpp"

#ifdef _WIN32

#include <windows.h>
#include <sstream>

namespace kvdb::torture {

ChildProcess::ChildProcess() noexcept
    : process_handle_( nullptr),
      stdin_write_(nullptr),
      stdout_read_(nullptr),
      stdout_buffer_() {}

ChildProcess::~ChildProcess() {
    kill_hard();
    wait_for_exit();
    close_stdin();
    if (stdout_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
    if (process_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
    }
}

bool ChildProcess::start(const std::string& exe_path, const std::vector<std::string>& args) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE child_stdout_rd = nullptr;
    HANDLE child_stdout_wr = nullptr;
    if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 1024 * 1024)) {
        return false;
    }
    SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE child_stdin_rd = nullptr;
    HANDLE child_stdin_wr = nullptr;
    if (!CreatePipe(&child_stdin_rd, &child_stdin_wr, &sa, 1024 * 1024)) {
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        return false;
    }
    SetHandleInformation(child_stdin_wr, HANDLE_FLAG_INHERIT, 0);

    std::ostringstream cmd_ss;
    cmd_ss << "\"" << exe_path << "\"";
    for (const auto& a : args) {
        cmd_ss << " \"" << a << "\"";
    }
    std::string cmd_str = cmd_ss.str();
    std::vector<char> cmd_buf(cmd_str.begin(), cmd_str.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb         = sizeof(STARTUPINFOA);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = child_stdout_wr;
    si.hStdInput  = child_stdin_rd;
    si.dwFlags   |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    // Close child-side handles in parent immediately so EOF works when child dies
    CloseHandle(child_stdout_wr);
    CloseHandle(child_stdin_rd);

    if (!ok) {
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdin_wr);
        return false;
    }

    CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;
    stdin_write_    = child_stdin_wr;
    stdout_read_    = child_stdout_rd;
    stdout_buffer_.clear();
    return true;
}

bool ChildProcess::write_stdin(const std::string& data) {
    if (stdin_write_ == nullptr) {
        return false;
    }
    const char* ptr = data.data();
    std::size_t rem = data.size();
    while (rem > 0) {
        DWORD chunk = static_cast<DWORD>(rem > 65536 ? 65536 : rem);
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(stdin_write_), ptr, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        ptr += written;
        rem -= written;
    }
    return true;
}

void ChildProcess::close_stdin() {
    if (stdin_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }
}

bool ChildProcess::read_stdout_line(std::string& out_line) {
    while (true) {
        auto nl_pos = stdout_buffer_.find('\n');
        if (nl_pos != std::string::npos) {
            out_line = stdout_buffer_.substr(0, nl_pos);
            if (!out_line.empty() && out_line.back() == '\r') {
                out_line.pop_back();
            }
            stdout_buffer_.erase(0, nl_pos + 1);
            return true;
        }

        if (stdout_read_ == nullptr) {
            return false;
        }

        char buf[4096];
        DWORD got = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(stdout_read_), buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) {
            if (!stdout_buffer_.empty()) {
                out_line = std::move(stdout_buffer_);
                stdout_buffer_.clear();
                if (!out_line.empty() && out_line.back() == '\r') {
                    out_line.pop_back();
                }
                return true;
            }
            return false;
        }
        stdout_buffer_.append(buf, got);
    }
}

void ChildProcess::kill_hard() {
    if (process_handle_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_handle_), 137);
    }
}

int ChildProcess::wait_for_exit() {
    if (process_handle_ == nullptr) {
        return 0;
    }
    WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &exit_code);
    CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
    return static_cast<int>(exit_code);
}

bool ChildProcess::is_running() const {
    if (process_handle_ == nullptr) {
        return false;
    }
    DWORD wait_res = WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0);
    return wait_res == WAIT_TIMEOUT;
}

} // namespace kvdb::torture

#endif // _WIN32