#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

namespace chuan::diagnostic {
inline double TransportNow(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline std::string Quote(const std::string &value){
    std::string out="\"";const char hex[]="0123456789abcdef";
    for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=static_cast<char>(c);}return out+'"';
}
inline std::string DetailedSocketInfo(SOCKET socket,bool basic=true){
    std::ostringstream out;TCP_INFO_v0 tcp{};ULONG version=0;DWORD returned=0;
    if(!WSAIoctl(socket,SIO_TCP_INFO,&version,sizeof(version),&tcp,sizeof(tcp),&returned,nullptr,nullptr)){
        if(basic)out<<",\"rtt_us\":"<<tcp.RttUs<<",\"cwnd_bytes\":"<<tcp.Cwnd<<",\"send_window_bytes\":"<<tcp.SndWnd<<",\"receive_window_bytes\":"<<tcp.RcvWnd<<",\"bytes_in_flight\":"<<tcp.BytesInFlight<<",\"retransmitted_bytes\":"<<tcp.BytesRetrans;
        out<<",\"tcp_rtt_min_us\":"<<tcp.MinRttUs<<",\"tcp_mss_bytes\":"<<tcp.Mss<<",\"tcp_age_ms\":"<<tcp.ConnectionTimeMs
            <<",\"tcp_receive_buffer_bytes\":"<<tcp.RcvBuf<<",\"tcp_bytes_out\":"<<tcp.BytesOut<<",\"tcp_bytes_in\":"<<tcp.BytesIn
            <<",\"tcp_fast_retransmits\":"<<tcp.FastRetrans<<",\"tcp_duplicate_acks\":"<<tcp.DupAcksIn<<",\"tcp_timeout_episodes\":"<<tcp.TimeoutEpisodes
            <<",\"tcp_syn_retransmits\":"<<static_cast<unsigned>(tcp.SynRetrans)<<",\"tcp_reordered_bytes\":"<<tcp.BytesReordered;
    }
    ULONG ideal=0;if(!WSAIoctl(socket,SIO_IDEAL_SEND_BACKLOG_QUERY,nullptr,0,&ideal,sizeof(ideal),&returned,nullptr,nullptr))out<<",\"ideal_send_backlog_bytes\":"<<ideal;
    else out<<",\"ideal_send_backlog_error\":"<<WSAGetLastError();
    for(auto [level,option,name]:{std::tuple{static_cast<int>(IPPROTO_TCP),TCP_NODELAY,"tcp_nodelay"},std::tuple{SOL_SOCKET,SO_SNDTIMEO,"socket_send_timeout_ms"},std::tuple{SOL_SOCKET,SO_RCVTIMEO,"socket_receive_timeout_ms"}}){
        int value=0,length=sizeof(value);if(!getsockopt(socket,level,option,reinterpret_cast<char*>(&value),&length))out<<",\""<<name<<"\":"<<value;
    }
    return out.str();
}
// This registry is observed by the log thread, so stalled I/O still produces snapshots.
class TransportMonitor {
    struct Connection {
        uint64_t id=0;double created=TransportNow();bool initialized=false;MIB_TCPROW row{};
        ULONG enableError=ERROR_NOT_SUPPORTED;uint64_t uses=0;std::string scope;
    };
    struct Activity {
        uint64_t connection=0,job=0,done=0,base=0,total=0,sampledBytes=0;
        double created=TransportNow(),progressAt=created,phaseAt=created,sampleAt=created,gaugeAt=created;
        std::string direction,extra,phase="initializing";std::map<std::string,uint64_t> gauges;
    };
    std::mutex mutex_;
    std::map<SOCKET,Connection> connections_;
    std::map<uint64_t,Activity> activities_;
    uint64_t nextConnection_=1,nextActivity_=1;
    std::function<void(const std::string&)> write_;
    ULONG interfaceError_=ULONG_MAX;
    Connection &RegisterLocked(SOCKET socket){
        auto [it,inserted]=connections_.try_emplace(socket);
        if(inserted){it->second.id=nextConnection_++;write_("\"event\":\"connection_open\",\"connection_id\":"+std::to_string(it->second.id));}
        return it->second;
    }
    void Initialize(SOCKET socket,Connection &connection){
        if(connection.initialized)return;connection.initialized=true;
        sockaddr_in local{},remote{};int length=sizeof(local);
        if(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&length))return;
        length=sizeof(remote);if(getpeername(socket,reinterpret_cast<sockaddr*>(&remote),&length))return;
        connection.row.dwLocalAddr=local.sin_addr.s_addr;connection.row.dwLocalPort=local.sin_port;
        connection.row.dwRemoteAddr=remote.sin_addr.s_addr;connection.row.dwRemotePort=remote.sin_port;connection.row.dwState=MIB_TCP_STATE_ESTAB;
        TCP_ESTATS_SND_CONG_RW_v0 rw{};rw.EnableCollection=TRUE;
        connection.enableError=SetPerTcpConnectionEStats(&connection.row,TcpConnectionEstatsSndCong,reinterpret_cast<PUCHAR>(&rw),0,sizeof(rw),0);
    }
    std::string EStats(Connection &connection){
        TCP_ESTATS_SND_CONG_RW_v0 rw{};TCP_ESTATS_SND_CONG_ROD_v0 stats{};
        const auto error=connection.initialized?GetPerTcpConnectionEStats(&connection.row,TcpConnectionEstatsSndCong,reinterpret_cast<PUCHAR>(&rw),0,sizeof(rw),nullptr,0,0,reinterpret_cast<PUCHAR>(&stats),0,sizeof(stats)):ERROR_NOT_SUPPORTED;
        std::ostringstream out;out<<",\"estats_enable_error\":"<<connection.enableError<<",\"estats_read_error\":"<<error<<",\"tcp_limit_counters_direction\":\"outbound\",\"estats_available\":"<<(!error&&rw.EnableCollection?"true":"false");
        if(!error&&rw.EnableCollection)out<<",\"tcp_receiver_limited_ms\":"<<stats.SndLimTimeRwin<<",\"tcp_congestion_limited_ms\":"<<stats.SndLimTimeCwnd<<",\"tcp_sender_limited_ms\":"<<stats.SndLimTimeSnd
            <<",\"tcp_receiver_limited_bytes\":"<<stats.SndLimBytesRwin<<",\"tcp_congestion_limited_bytes\":"<<stats.SndLimBytesCwnd<<",\"tcp_sender_limited_bytes\":"<<stats.SndLimBytesSnd
            <<",\"tcp_receiver_limited_transitions\":"<<stats.SndLimTransRwin<<",\"tcp_congestion_limited_transitions\":"<<stats.SndLimTransCwnd<<",\"tcp_sender_limited_transitions\":"<<stats.SndLimTransSnd
            <<",\"tcp_slow_starts\":"<<stats.SlowStart<<",\"tcp_congestion_avoidance\":"<<stats.CongAvoid<<",\"tcp_other_window_reductions\":"<<stats.OtherReductions<<",\"tcp_ssthresh_bytes\":"<<stats.CurSsthresh;
        return out.str();
    }
  public:
    explicit TransportMonitor(std::function<void(const std::string&)> write):write_(std::move(write)){}
    void SampleInterfaces()noexcept{try{
        MIB_IF_TABLE2 *table=nullptr;const auto error=GetIfTable2(&table);
        if(error){if(error!=interfaceError_)write_("\"event\":\"host_interface_error\",\"error\":"+std::to_string(error));interfaceError_=error;return;}
        const auto release=std::unique_ptr<MIB_IF_TABLE2,decltype(&FreeMibTable)>(table,&FreeMibTable);interfaceError_=0;
        for(ULONG i=0;i<table->NumEntries;++i){const auto &row=table->Table[i];if(row.OperStatus!=IfOperStatusUp||row.Type==IF_TYPE_SOFTWARE_LOOPBACK)continue;
            std::ostringstream out;out<<"\"event\":\"host_interface_sample\",\"scope\":\"all_host_traffic\",\"interface_index\":"<<row.InterfaceIndex<<",\"interface_type\":"<<row.Type<<",\"transmit_link_bps\":"<<row.TransmitLinkSpeed<<",\"receive_link_bps\":"<<row.ReceiveLinkSpeed
                <<",\"rx_bytes\":"<<row.InOctets<<",\"tx_bytes\":"<<row.OutOctets<<",\"rx_discard_packets\":"<<row.InDiscards<<",\"tx_discard_packets\":"<<row.OutDiscards<<",\"rx_error_packets\":"<<row.InErrors<<",\"tx_error_packets\":"<<row.OutErrors;write_(out.str());
        }
    }catch(...){}}
    void Register(SOCKET socket)noexcept{try{std::lock_guard lock(mutex_);RegisterLocked(socket);}catch(...){}}
    void Close(SOCKET socket)noexcept{try{std::lock_guard lock(mutex_);auto it=connections_.find(socket);if(it==connections_.end())return;
        write_("\"event\":\"connection_close\",\"connection_id\":"+std::to_string(it->second.id)+",\"connection_age_ms\":"+std::to_string(TransportNow()-it->second.created)+",\"payload_uses\":"+std::to_string(it->second.uses)+it->second.scope+(it->second.initialized?DetailedSocketInfo(socket,false)+EStats(it->second):""));connections_.erase(it);
    }catch(...){}}
    std::pair<uint64_t,uint64_t> Begin(SOCKET socket,uint64_t job,uint64_t total,const std::string &direction,const std::string &extra)noexcept{
        try{std::lock_guard lock(mutex_);auto &connection=RegisterLocked(socket);Initialize(socket,connection);++connection.uses;connection.scope=",\"id\":"+std::to_string(job)+extra;
            const auto id=nextActivity_++;Activity value;value.connection=connection.id;value.job=job;value.total=total;value.direction=direction;value.extra=extra;activities_[id]=std::move(value);return {id,connection.id};
        }catch(...){return {};}
    }
    void Progress(uint64_t id,uint64_t done,bool baseline=false)noexcept{try{std::lock_guard lock(mutex_);auto it=activities_.find(id);if(it!=activities_.end()){auto &value=it->second;if(done>value.done)value.progressAt=TransportNow();value.done=done;if(baseline){value.base=done;value.sampledBytes=done;}}}catch(...){}}
    void Phase(uint64_t id,const std::string &phase)noexcept{try{std::lock_guard lock(mutex_);auto it=activities_.find(id);if(it!=activities_.end()){it->second.phase=phase;it->second.phaseAt=TransportNow();}}catch(...){}}
    void Gauge(uint64_t id,const std::string &name,uint64_t value)noexcept{try{std::lock_guard lock(mutex_);auto it=activities_.find(id);if(it!=activities_.end()){it->second.gauges[name]=value;if(name=="send_oldest_operation_ms")it->second.gaugeAt=TransportNow();}}catch(...){}}
    void End(uint64_t id)noexcept{try{std::lock_guard lock(mutex_);activities_.erase(id);}catch(...){}}
    std::string Snapshot(SOCKET socket)noexcept{try{std::lock_guard lock(mutex_);auto it=connections_.find(socket);return it==connections_.end()?"":DetailedSocketInfo(socket,false)+EStats(it->second);}catch(...){return ",\"transport_snapshot_error\":true";}}
    void Sample()noexcept{try{std::lock_guard lock(mutex_);const auto now=TransportNow();std::map<uint64_t,unsigned> concurrency;
        for(const auto &[id,value]:activities_)if(value.extra.find("\"socket_role\":\"control\"")==std::string::npos)++concurrency[value.job];
        for(auto &[id,value]:activities_){
            std::string gauges;for(const auto &[name,number]:value.gauges)gauges+=","+Quote(name)+":"+std::to_string(number+(name=="send_oldest_operation_ms"&&value.gauges.contains("send_pending_operations")&&value.gauges.at("send_pending_operations")?static_cast<uint64_t>(std::max(0.0,now-value.gaugeAt)):0));
            write_("\"event\":\"payload_activity\",\"trace_id\":"+std::to_string(id)+",\"id\":"+std::to_string(value.job)+",\"connection_id\":"+std::to_string(value.connection)+",\"direction\":"+Quote(value.direction)+",\"phase\":"+Quote(value.phase)+",\"phase_age_ms\":"+std::to_string(now-value.phaseAt)+",\"progress_idle_ms\":"+std::to_string(now-value.progressAt)+",\"done_bytes\":"+std::to_string(value.done)+",\"interval_bytes\":"+std::to_string(value.done>=value.sampledBytes?value.done-value.sampledBytes:0)+",\"interval_ms\":"+std::to_string(now-value.sampleAt)+",\"active_data_traces_for_job\":"+std::to_string(concurrency[value.job])+gauges+value.extra);
            value.sampledBytes=value.done;value.sampleAt=now;
        }
        for(auto &[socket,connection]:connections_){bool active=false;for(const auto &[id,value]:activities_)if(value.connection==connection.id){active=true;break;}
            if(active)write_("\"event\":\"tcp_connection_sample\",\"connection_id\":"+std::to_string(connection.id)+",\"payload_uses\":"+std::to_string(connection.uses)+connection.scope+DetailedSocketInfo(socket)+EStats(connection));
        }
    }catch(...){}}
};
}
