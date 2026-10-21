#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <algorithm>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <psapi.h>
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
    std::mutex mutex_,ioMutex_;std::condition_variable changed_,flushed_;
    std::filesystem::path directory_,path_;std::ofstream stream_;std::thread writer_;
    std::deque<std::string> queue_;bool stopping_=false,busy_=false;uint64_t bytes_=0,segment_=0,submitted_=0,completed_=0;
    std::atomic<uint64_t> dropped_{0},ioErrors_{0};std::string prefix_;uint64_t resourceAt_=0;
    void Run() noexcept {
        for(;;){
            std::deque<std::string> batch;
            {std::unique_lock lock(mutex_);changed_.wait_for(lock,std::chrono::milliseconds(250),[&]{return stopping_||queue_.size()>=128;});
             if(stopping_&&queue_.empty())break;batch.swap(queue_);busy_=true;}
            try{std::lock_guard io(ioMutex_);
                for(const auto &line:batch){
                    if(bytes_>=16*1024*1024){stream_.close();path_=directory_/(prefix_+"-"+std::to_string(++segment_)+".jsonl");stream_.open(path_,std::ios::binary);bytes_=0;}
                    if(stream_){stream_<<line;bytes_+=line.size();if(!stream_){dropped_++;ioErrors_++;}}else dropped_++;
                }stream_.flush();if(!stream_)ioErrors_++;
            }catch(...){dropped_+=batch.size();ioErrors_++;}
            if(GetTickCount64()-resourceAt_>=1000){
                resourceAt_=GetTickCount64();FILETIME created{},ended{},kernel{},user{};PROCESS_MEMORY_COUNTERS memory{};MEMORYSTATUSEX available{};available.dwLength=sizeof(available);
                if(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)&&GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))){
                    const uint64_t cpu=((static_cast<uint64_t>(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+((static_cast<uint64_t>(user.dwHighDateTime)<<32)|user.dwLowDateTime);
                    GlobalMemoryStatusEx(&available);SYSTEM_INFO system{};GetSystemInfo(&system);
                    Write("\"event\":\"resources\",\"process_cpu_ms\":"+std::to_string(cpu/10000)+",\"working_set_bytes\":"+std::to_string(memory.WorkingSetSize)+",\"physical_available_bytes\":"+std::to_string(available.ullAvailPhys)+",\"cpu_count\":"+std::to_string(system.dwNumberOfProcessors));
                }
            }
            {std::lock_guard lock(mutex_);busy_=false;completed_+=batch.size();}flushed_.notify_all();
        }
    }
  public:
    ~Log(){Write("\"event\":\"session_end\",\"dropped_events\":"+std::to_string(dropped_.load())+",\"storage_errors\":"+std::to_string(ioErrors_.load())); {std::lock_guard lock(mutex_);stopping_=true;}changed_.notify_all();if(writer_.joinable())writer_.join();}
    bool Open(const std::filesystem::path &directory) noexcept {
        try{if(writer_.joinable())return true;directory_=directory;std::filesystem::create_directories(directory_);
            prefix_="transfer-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
            path_=directory_/(prefix_+"-0.jsonl");stream_.open(path_,std::ios::binary);if(!stream_)return false;
            writer_=std::thread([this]{Run();});return true;
        }catch(...){return false;}
    }
    void Write(const std::string &fields) noexcept {
        try{SYSTEMTIME utc{};GetSystemTime(&utc);std::ostringstream line;
            line<<"{\"utc\":\""<<std::setfill('0')<<std::setw(4)<<utc.wYear<<'-'<<std::setw(2)<<utc.wMonth<<'-'<<std::setw(2)<<utc.wDay<<'T'<<std::setw(2)<<utc.wHour<<':'<<std::setw(2)<<utc.wMinute<<':'<<std::setw(2)<<utc.wSecond<<'.'<<std::setw(3)<<utc.wMilliseconds<<"Z\",\"diagnostic_session\":\""<<prefix_<<"\",\"mono_ms\":"<<std::fixed<<std::setprecision(3)<<Milliseconds()<<",\"platform\":\"windows\","<<fields<<"}\n";
            {std::lock_guard lock(mutex_);if(!writer_.joinable()||stopping_||queue_.size()>=16384){dropped_++;return;}queue_.push_back(line.str());++submitted_;}
            changed_.notify_one();
        }catch(...){dropped_++;}
    }
    void Flush(){std::unique_lock lock(mutex_);if(!writer_.joinable())return;changed_.notify_all();const auto fence=submitted_;if(!flushed_.wait_for(lock,std::chrono::seconds(10),[&]{return completed_>=fence;}))throw std::runtime_error("diagnostic flush timeout");}
    bool Export(const std::filesystem::path &target,const std::string &clientJson="") noexcept {
        try{Flush();std::lock_guard io(ioMutex_);stream_.flush();
            std::vector<std::filesystem::directory_entry> files;
            for(const auto &entry:std::filesystem::directory_iterator(directory_))if(entry.is_regular_file()&&entry.path().filename().wstring().starts_with(L"transfer-")&&entry.path().extension()==L".jsonl")files.push_back(entry);
            std::sort(files.begin(),files.end(),[](const auto &a,const auto &b){return a.last_write_time()>b.last_write_time();});
            uint64_t retained=0;size_t selected=0;for(const auto &f:files){if(retained+f.file_size()>256ull*1024*1024)break;retained+=f.file_size();++selected;}
            std::ofstream out(target,std::ios::binary);if(!out)return false;
            out<<"{\"event\":\"export_manifest\",\"diagnostic_schema\":2,\"platform\":\"windows\",\"dropped_events\":"<<dropped_<<",\"storage_errors\":"<<ioErrors_<<",\"omitted_segments\":"<<files.size()-selected<<",\"selected_segments\":"<<selected<<"}\n";
            for(size_t i=selected;i>0;--i){std::ifstream input(files[i-1].path(),std::ios::binary);std::string line;
                while(std::getline(input,line))if(clientJson.empty()||line.find("\"event\":\"session_start\"")!=std::string::npos||line.find("\"event\":\"resources\"")!=std::string::npos||line.find("\"client_id\":"+clientJson)!=std::string::npos||line.find("\"recipient_id\":"+clientJson)!=std::string::npos)out<<line<<'\n';}
            out.flush();return out.good();
        }catch(...){return false;}
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
    std::string extra_;
    double start_ = Milliseconds(), sample_ = start_;
    uint64_t sampledBytes_ = 0;bool ended_=false;
  public:
    double networkMs = 0, diskMs = 0, hashMs = 0, flushMs = 0;
    uint64_t done = 0;
    std::string result = "failed", failureStage;
    int socketError = 0; DWORD fileError = 0;
    Transfer(Log &log, SOCKET socket, uint64_t id, uint64_t total, const char *direction,
             const std::string &extra = {}) : log_(log), socket_(socket), id_(id), extra_(extra) {
        log_.Write("\"event\":\"transfer_start\",\"id\":" + std::to_string(id_) +
                   ",\"direction\":\"" + direction + "\",\"total_bytes\":" + std::to_string(total) +
                   SocketInfo(socket_) + extra);
    }
    void StartAt(uint64_t offset){done=offset;sampledBytes_=offset;}
    void Sample(bool final = false) noexcept {
        try {
            if(final&&ended_)return;if(final)ended_=true;
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
            fields << extra_ << SocketInfo(socket_); log_.Write(fields.str()); sample_ = now; sampledBytes_ = done;
        } catch (...) {}
    }
    ~Transfer() { Sample(true); }
};
}
