#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace chuan {
// A bounded reorder window decouples four HTTP upload connections from ordered disk writes.
struct ParallelUpload {
    static constexpr uint64_t SegmentBytes=8*1024*1024;
    std::string client,upload,batch;
    uint64_t total=0,start=0,next=0,created=0,transferId=0;
    size_t consumed=0;
    bool attached=false,closed=false;
    std::mutex mutex;
    std::condition_variable changed;
    std::set<uint64_t> reserved;
    std::map<uint64_t,std::vector<char>> ready;
    std::function<void(const std::string&)> observer;
    std::chrono::steady_clock::time_point lastObserved{};
    void Observe(const char *event,bool force=true)noexcept{try{const auto now=std::chrono::steady_clock::now();if(!observer||(!force&&now-lastObserved<std::chrono::seconds(1)))return;lastObserved=now;uint64_t buffered=0;for(const auto &[offset,bytes]:ready)buffered+=bytes.size();
        observer("\"queue_event\":\""+std::string(event)+"\",\"next_offset\":"+std::to_string(next)+",\"reserved_segments\":"+std::to_string(reserved.size())+",\"ready_segments\":"+std::to_string(ready.size())+",\"buffered_bytes\":"+std::to_string(buffered)+",\"partial_segment_bytes\":"+std::to_string(consumed)+",\"next_segment_ready\":"+(ready.contains(next)?"true":"false"));}catch(...){}}
    void SetObserver(std::function<void(const std::string&)> value){std::lock_guard lock(mutex);observer=std::move(value);}
    void Close(){std::lock_guard lock(mutex);closed=true;changed.notify_all();}
    void Attach(uint64_t id){std::lock_guard lock(mutex);if(closed||attached)throw std::runtime_error("upload session");transferId=id;attached=true;changed.notify_all();}
    void Reserve(uint64_t offset,uint64_t length){
        std::unique_lock lock(mutex);
        if(offset<start||offset>=total||(offset-start)%SegmentBytes||length!=std::min(SegmentBytes,total-offset))throw std::runtime_error("segment bounds");
        if(!changed.wait_for(lock,std::chrono::seconds(15),[&]{return closed||(attached&&offset>=next&&offset-next<4*SegmentBytes&&reserved.size()<4);})||closed||reserved.contains(offset))throw std::runtime_error("upload window");
        reserved.insert(offset);Observe("reserve");
    }
    void Put(uint64_t offset,std::vector<char> bytes){std::lock_guard lock(mutex);if(closed||!reserved.contains(offset))throw std::runtime_error("upload session");ready.emplace(offset,std::move(bytes));Observe("segment_ready");changed.notify_all();}
    size_t Read(char *destination,size_t length,const std::function<bool()> &active){
        std::unique_lock lock(mutex);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!closed&&!ready.contains(next)&&active()&&std::chrono::steady_clock::now()<deadline){Observe("ordered_read_wait",false);changed.wait_for(lock,std::chrono::milliseconds(100));}
        if(closed||!active()||!ready.contains(next))throw std::runtime_error("parallel upload interrupted");
        auto &bytes=ready.at(next);const auto count=std::min(length,bytes.size()-consumed);std::copy_n(bytes.data()+consumed,count,destination);consumed+=count;
        if(consumed==bytes.size()){const auto offset=next;next+=bytes.size();consumed=0;ready.erase(offset);reserved.erase(offset);Observe("segment_consumed");changed.notify_all();}
        return count;
    }
};
}
