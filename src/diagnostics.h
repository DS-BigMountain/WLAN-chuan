#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

namespace chuan::diagnostic {
inline double Milliseconds() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Logging is best effort and never changes the outcome of a transfer.
class Log {
    std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream stream_;
    uint64_t bytes_ = 0;
  public:
    bool Open(const std::filesystem::path &directory) noexcept {
        try {
            std::filesystem::create_directories(directory);
            path_ = directory / (L"transfer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L".jsonl");
            stream_.open(path_, std::ios::binary | std::ios::app);
            return stream_.good();
        } catch (...) { return false; }
    }
    void Write(const std::string &fields) noexcept {
        try {
            std::lock_guard lock(mutex_);
            if (!stream_) return;
            // Retain one previous segment; rotate only this process's own log.
            if (bytes_ >= 8 * 1024 * 1024) {
                stream_.close();
                auto previous = path_; previous += L".previous";
                std::error_code ec; std::filesystem::remove(previous, ec);
                ec.clear(); std::filesystem::rename(path_, previous, ec);
                if (ec) return;
                stream_.open(path_, std::ios::binary | std::ios::trunc); bytes_ = 0;
            }
            SYSTEMTIME utc{}; GetSystemTime(&utc);
            std::ostringstream line;
            line << "{\"utc\":\"" << std::setfill('0') << std::setw(4) << utc.wYear << '-'
                 << std::setw(2) << utc.wMonth << '-' << std::setw(2) << utc.wDay << 'T'
                 << std::setw(2) << utc.wHour << ':' << std::setw(2) << utc.wMinute << ':'
                 << std::setw(2) << utc.wSecond << '.' << std::setw(3) << utc.wMilliseconds
                 << "Z\"," << fields << "}\n";
            const auto text = line.str(); stream_ << text; stream_.flush(); bytes_ += text.size();
        } catch (...) {}
    }
};
inline std::string SocketInfo(SOCKET socket) {
    std::ostringstream out;
    sockaddr_in local{}, remote{}; int length = sizeof(local); char address[64]{};
    if (!getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length)) {
        InetNtopA(AF_INET, &local.sin_addr, address, sizeof(address));
        out << ",\"local_ip\":\"" << address << "\",\"local_port\":" << ntohs(local.sin_port);
    }
    length = sizeof(remote);
    if (!getpeername(socket, reinterpret_cast<sockaddr *>(&remote), &length)) {
        InetNtopA(AF_INET, &remote.sin_addr, address, sizeof(address));
        out << ",\"remote_ip\":\"" << address << "\",\"remote_port\":" << ntohs(remote.sin_port);
    }
    for (auto [option, name] : {std::pair{SO_SNDBUF, "send_buffer_bytes"}, std::pair{SO_RCVBUF, "receive_buffer_bytes"}}) {
        int value = 0; length = sizeof(value);
        if (!getsockopt(socket, SOL_SOCKET, option, reinterpret_cast<char *>(&value), &length))
            out << ",\"" << name << "\":" << value;
    }
    TCP_INFO_v0 tcp{}; ULONG version = 0; DWORD returned = 0;
    if (!WSAIoctl(socket, SIO_TCP_INFO, &version, sizeof(version), &tcp, sizeof(tcp), &returned, nullptr, nullptr)) {
        out << ",\"rtt_us\":" << tcp.RttUs << ",\"cwnd_bytes\":" << tcp.Cwnd
            << ",\"send_window_bytes\":" << tcp.SndWnd << ",\"receive_window_bytes\":" << tcp.RcvWnd
            << ",\"bytes_in_flight\":" << tcp.BytesInFlight << ",\"retransmitted_bytes\":" << tcp.BytesRetrans;
    } else out << ",\"tcp_info_error\":" << WSAGetLastError();
    return out.str();
}
struct Measure {
    double &sum; double start = Milliseconds();
    explicit Measure(double &target) : sum(target) {}
    ~Measure() { sum += Milliseconds() - start; }
};
class Transfer {
    Log &log_; SOCKET socket_; uint64_t id_;
    double start_ = Milliseconds(), sample_ = start_;
    uint64_t sampledBytes_ = 0;
  public:
    double networkMs = 0, diskMs = 0, hashMs = 0, flushMs = 0;
    uint64_t done = 0;
    std::string result = "failed", failureStage;
    int socketError = 0; DWORD fileError = 0;
    Transfer(Log &log, SOCKET socket, uint64_t id, uint64_t total, const char *direction,
             const std::string &extra = {}) : log_(log), socket_(socket), id_(id) {
        log_.Write("\"event\":\"transfer_start\",\"id\":" + std::to_string(id_) +
                   ",\"direction\":\"" + direction + "\",\"total_bytes\":" + std::to_string(total) +
                   SocketInfo(socket_) + extra);
    }
    void Sample(bool final = false) noexcept {
        try {
            const double now = Milliseconds();
            if (!final && now - sample_ < 1000) return;
            std::ostringstream fields;
            fields << std::setprecision(12) << "\"event\":\"" << (final ? "transfer_end" : "transfer_sample")
                   << "\",\"id\":" << id_ << ",\"done_bytes\":" << done
                   << ",\"elapsed_ms\":" << now - start_ << ",\"interval_ms\":" << now - sample_
                   << ",\"interval_bytes\":" << done - sampledBytes_
                   << ",\"network_call_ms\":" << networkMs << ",\"disk_call_ms\":" << diskMs
                   << ",\"hash_ms\":" << hashMs << ",\"flush_ms\":" << flushMs;
            if (final) fields << ",\"result\":\"" << result << "\",\"failure_stage\":\"" << failureStage
                              << "\",\"socket_error\":" << socketError << ",\"file_error\":" << fileError;
            fields << SocketInfo(socket_); log_.Write(fields.str()); sample_ = now; sampledBytes_ = done;
        } catch (...) {}
    }
    ~Transfer() { Sample(true); }
};
}
