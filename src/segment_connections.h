#pragma once
#include <winsock2.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
namespace chuan {
// Connections survive segment boundaries; leases serialize each HTTP stream.
class SegmentConnections {
    std::array<SOCKET,4> sockets_{INVALID_SOCKET,INVALID_SOCKET,INVALID_SOCKET,INVALID_SOCKET};
    std::array<bool,4> busy_{};
    std::mutex mutex_;
    std::condition_variable available_;
    std::function<void(SOCKET)> close_;
  public:
    explicit SegmentConnections(std::function<void(SOCKET)> close):close_(std::move(close)){}
    ~SegmentConnections(){for(auto socket:sockets_)if(socket!=INVALID_SOCKET)close_(socket);}
    size_t Acquire(const std::function<bool()> &active){
        std::unique_lock lock(mutex_);
        for(;;){
            if(!active())throw std::runtime_error("cancel");
            for(size_t i=0;i<busy_.size();++i)if(!busy_[i]){busy_[i]=true;return i;}
            available_.wait_for(lock,std::chrono::milliseconds(100));
        }
    }
    SOCKET &Socket(size_t index){return sockets_[index];}
    void Release(size_t index,bool healthy){
        std::lock_guard lock(mutex_);
        if(!healthy&&sockets_[index]!=INVALID_SOCKET){close_(sockets_[index]);sockets_[index]=INVALID_SOCKET;}
        busy_[index]=false;available_.notify_one();
    }
};
}
