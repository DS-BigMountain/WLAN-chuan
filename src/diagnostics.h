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
#include <set>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "transport_diagnostics.h"

namespace chuan::diagnostic {
inline double Milliseconds() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Logging is best effort and never changes the outcome of a transfer.
class Log {
    std::mutex mutex_,ioMutex_;std::condition_variable changed_,flushed_;
    std::filesystem::path directory_,path_;std::ofstream stream_;std::thread writer_;
    struct Entry{uint64_t epoch;std::string text;};
    std::deque<Entry> queue_;std::atomic<uint64_t> epoch_{0};bool stopping_=false,busy_=false;uint64_t bytes_=0,segment_=0,submitted_=0,completed_=0;
    std::atomic<uint64_t> dropped_{0},ioErrors_{0};std::string prefix_;uint64_t resourceAt_=0;
    std::mutex exportMutex_;
    std::map<std::string,std::map<std::string,uint64_t>> exportCursors_;
    std::map<std::wstring,std::pair<std::string,std::map<std::string,uint64_t>>> pendingExports_;
    std::atomic<uint64_t> recordId_{0};
    void SaveCursors(){
        const auto temporary=directory_/L"export-cursors.tmp",target=directory_/L"export-cursors.state";
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        for(const auto &[owner,files]:exportCursors_){if(files.empty())output<<std::quoted(owner)<<" \"\" 0\n";for(const auto &[file,offset]:files)output<<std::quoted(owner)<<' '<<std::quoted(file)<<' '<<offset<<'\n';}
        output.flush();if(!output)throw std::runtime_error("cursor write");output.close();
        if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cursor commit");
    }
    void Run() noexcept {
        for(;;){
            std::deque<Entry> batch;
            {std::unique_lock lock(mutex_);changed_.wait_for(lock,std::chrono::milliseconds(250),[&]{return stopping_||queue_.size()>=128;});
             if(stopping_&&queue_.empty())break;batch.swap(queue_);busy_=true;}
            try{std::lock_guard io(ioMutex_);
                for(const auto &entry:batch){
                    if(entry.epoch!=epoch_.load())continue;const auto &line=entry.text;
                    if(bytes_>=16*1024*1024){stream_.close();path_=directory_/(prefix_+"-"+std::to_string(++segment_)+".jsonl");stream_.open(path_,std::ios::binary);bytes_=0;}
                    if(stream_){stream_<<line;bytes_+=line.size();if(!stream_){dropped_++;ioErrors_++;}}else dropped_++;
                }stream_.flush();if(!stream_)ioErrors_++;
            }catch(...){dropped_+=batch.size();ioErrors_++;}
            if(GetTickCount64()-resourceAt_>=1000){
                resourceAt_=GetTickCount64();FILETIME created{},ended{},kernel{},user{};PROCESS_MEMORY_COUNTERS memory{};MEMORYSTATUSEX available{};available.dwLength=sizeof(available);
                if(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)&&GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))){
                    const uint64_t cpu=((static_cast<uint64_t>(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+((static_cast<uint64_t>(user.dwHighDateTime)<<32)|user.dwLowDateTime);
                    GlobalMemoryStatusEx(&available);SYSTEM_INFO system{};GetSystemInfo(&system);
                    IO_COUNTERS io{};GetProcessIoCounters(GetCurrentProcess(),&io);
                    Write("\"event\":\"resources\",\"process_cpu_ms\":"+std::to_string(cpu/10000)+",\"working_set_bytes\":"+std::to_string(memory.WorkingSetSize)+",\"physical_available_bytes\":"+std::to_string(available.ullAvailPhys)+",\"cpu_count\":"+std::to_string(system.dwNumberOfProcessors)+",\"process_io_read_bytes\":"+std::to_string(io.ReadTransferCount)+",\"process_io_write_bytes\":"+std::to_string(io.WriteTransferCount)+",\"process_io_other_bytes\":"+std::to_string(io.OtherTransferCount));
                }
                transport.Sample();
                transport.SampleInterfaces();
            }
            {std::lock_guard lock(mutex_);busy_=false;completed_+=batch.size();}flushed_.notify_all();
        }
    }
  public:
    TransportMonitor transport{[this](const std::string &fields){Write(fields);}};
    uint64_t Epoch()const noexcept{return epoch_.load();}
    ~Log(){Write("\"event\":\"session_end\",\"dropped_events\":"+std::to_string(dropped_.load())+",\"storage_errors\":"+std::to_string(ioErrors_.load())); {std::lock_guard lock(mutex_);stopping_=true;}changed_.notify_all();if(writer_.joinable())writer_.join();}
    bool Open(const std::filesystem::path &directory) noexcept {
        try{if(writer_.joinable())return true;directory_=directory;std::filesystem::create_directories(directory_);
            {std::ifstream state(directory_/L"export-cursors.state");std::string owner,file;uint64_t offset=0;while(state>>std::quoted(owner)>>std::quoted(file)>>offset){auto &cursor=exportCursors_[owner];if(!file.empty())cursor[file]=offset;}}
            if(!exportCursors_.contains("__initial__")){
                auto &baseline=exportCursors_["__initial__"];
                for(const auto &entry:std::filesystem::directory_iterator(directory_))if(entry.is_regular_file()&&entry.path().filename().wstring().starts_with(L"transfer-")&&entry.path().extension()==L".jsonl")baseline[entry.path().filename().string()]=entry.file_size();
                try{SaveCursors();}catch(...){++ioErrors_;}
            }
            prefix_="transfer-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
            path_=directory_/(prefix_+"-0.jsonl");stream_.open(path_,std::ios::binary);if(!stream_)return false;
            writer_=std::thread([this]{Run();});return true;
        }catch(...){return false;}
    }
    void Write(const std::string &fields,uint64_t expectedEpoch=UINT64_MAX) noexcept {
        try{const auto generation=epoch_.load();if(expectedEpoch!=UINT64_MAX&&generation!=expectedEpoch)return;SYSTEMTIME utc{};GetSystemTime(&utc);std::ostringstream line;
            line<<"{\"utc\":\""<<std::setfill('0')<<std::setw(4)<<utc.wYear<<'-'<<std::setw(2)<<utc.wMonth<<'-'<<std::setw(2)<<utc.wDay<<'T'<<std::setw(2)<<utc.wHour<<':'<<std::setw(2)<<utc.wMinute<<':'<<std::setw(2)<<utc.wSecond<<'.'<<std::setw(3)<<utc.wMilliseconds<<"Z\",\"diagnostic_session\":\""<<prefix_<<"\",\"mono_ms\":"<<std::fixed<<std::setprecision(3)<<Milliseconds()<<",\"platform\":\"windows\","<<"\"record_id\":"<<++recordId_<<",\"log_epoch\":"<<generation<<",\"thread_id\":"<<GetCurrentThreadId()<<","<<fields<<"}\n";
            {std::lock_guard lock(mutex_);if(!writer_.joinable()||stopping_||queue_.size()>=16384){dropped_++;return;}queue_.push_back({generation,line.str()});++submitted_;}
            changed_.notify_one();
        }catch(...){dropped_++;}
    }
    void Flush(){std::unique_lock lock(mutex_);if(!writer_.joinable())return;changed_.notify_all();const auto fence=submitted_;if(!flushed_.wait_for(lock,std::chrono::seconds(10),[&]{return completed_>=fence;}))throw std::runtime_error("diagnostic flush timeout");}
#include "diagnostic_export.inc"
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
    uint64_t traceId_=0,connectionId_=0;
    const char *phase_="between_operations";
    struct Timing{uint64_t calls=0;double ms=0,maxMs=0;std::array<uint64_t,7> buckets{};};
    std::map<std::string,Timing> timings_;
  public:
    class Operation {
        Transfer &trace_;double &sum_;const char *name_,*previous_;double start_=Milliseconds();
      public:
        Operation(Transfer &trace,const char *name,double &sum):trace_(trace),sum_(sum),name_(name),previous_(trace.phase_){trace_.phase_=name;trace_.log_.transport.Phase(trace_.traceId_,name);}
        Operation(const Operation&)=delete;
        ~Operation(){try{const auto elapsed=Milliseconds()-start_;sum_+=elapsed;auto &metric=trace_.timings_[name_];++metric.calls;metric.ms+=elapsed;metric.maxMs=std::max(metric.maxMs,elapsed);const double limits[]={1,5,20,100,500,1000};size_t bucket=0;while(bucket<6&&elapsed>limits[bucket])++bucket;++metric.buckets[bucket];trace_.phase_=previous_;trace_.log_.transport.Phase(trace_.traceId_,previous_);}catch(...){}}
    };
    Operation Stage(const char *name,double &sum){return Operation(*this,name,sum);}
    void Gauge(const std::string &name,uint64_t value)noexcept{log_.transport.Gauge(traceId_,name,value);}
    std::string Context()const{return "\"id\":"+std::to_string(id_)+extra_;}
    void Note(const std::string &event,const std::string &fields)noexcept{try{log_.Write("\"event\":"+Quote(event)+","+Context()+(fields.empty()?"":","+fields));}catch(...){}}
    double networkMs = 0, diskMs = 0, hashMs = 0, flushMs = 0;
    uint64_t done = 0, receiveCalls = 0, diskWriteCalls = 0;
    std::string result = "failed", failureStage;
    int socketError = 0; DWORD fileError = 0;
    Transfer(Log &log, SOCKET socket, uint64_t id, uint64_t total, const char *direction,
             const std::string &extra = {}) : log_(log), socket_(socket), id_(id), extra_(extra) {
        extra_+=extra.find("_mode\":\"parallel_")!=std::string::npos?",\"socket_role\":\"control\"":",\"socket_role\":\"data\"";
        const auto identity=log_.transport.Begin(socket,id,total,direction,extra_);traceId_=identity.first;connectionId_=identity.second;
        extra_+=",\"trace_id\":"+std::to_string(traceId_)+",\"connection_id\":"+std::to_string(connectionId_)+",\"direction\":"+Quote(direction);
        log_.Write("\"event\":\"transfer_start\",\"id\":" + std::to_string(id_) +
                   ",\"total_bytes\":" + std::to_string(total) +
                   SocketInfo(socket_) + extra_+log_.transport.Snapshot(socket_));
    }
    void StartAt(uint64_t offset){done=offset;sampledBytes_=offset;log_.transport.Progress(traceId_,done,true);}
    void Sample(bool final = false) noexcept {
        try {
            if(final&&ended_)return;if(final)ended_=true;
            log_.transport.Progress(traceId_,done);
            const double now = Milliseconds();
            if (!final && now - sample_ < 1000) return;
            std::ostringstream fields;
            fields << std::setprecision(12) << "\"event\":\"" << (final ? "transfer_end" : "transfer_sample")
                   << "\",\"id\":" << id_ << ",\"done_bytes\":" << done
                   << ",\"elapsed_ms\":" << now - start_ << ",\"interval_ms\":" << now - sample_
                   << ",\"interval_bytes\":" << done - sampledBytes_
                   << ",\"network_call_ms\":" << networkMs << ",\"disk_call_ms\":" << diskMs
                   << ",\"hash_ms\":" << hashMs << ",\"flush_ms\":" << flushMs;
            if (receiveCalls || diskWriteCalls)
                fields << ",\"receive_calls\":" << receiveCalls << ",\"disk_write_calls\":" << diskWriteCalls;
            if (final) fields << ",\"result\":" << Quote(result) << ",\"failure_stage\":" << Quote(failureStage)
                              << ",\"socket_error\":" << socketError << ",\"file_error\":" << fileError;
            fields<<",\"operation_timings\":{";bool comma=false;
            for(const auto &[name,metric]:timings_){if(comma)fields<<',';comma=true;fields<<Quote(name)<<":{\"calls\":"<<metric.calls<<",\"total_ms\":"<<metric.ms<<",\"max_ms\":"<<metric.maxMs<<",\"latency_buckets_ms_1_5_20_100_500_1000_inf\":[";for(size_t i=0;i<metric.buckets.size();++i){if(i)fields<<',';fields<<metric.buckets[i];}fields<<"]}";}fields<<'}';
            fields << extra_ << SocketInfo(socket_) << log_.transport.Snapshot(socket_); log_.Write(fields.str()); sample_ = now; sampledBytes_ = done;
        } catch (...) {}
    }
    ~Transfer() { Sample(true);log_.transport.End(traceId_); }
};
}
