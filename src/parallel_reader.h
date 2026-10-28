#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace chuan {
// Four segments are fetched ahead of the current segment, independent of file size.
class ParallelReader {
    using Fetch = std::function<std::vector<char>(uint64_t, uint64_t)>;
    Fetch fetch_;
    uint64_t next_, end_,ordered_;
    std::deque<std::future<std::vector<char>>> pending_;
    std::vector<char> current_;
    size_t consumed_ = 0;
    std::function<void(const char*,uint64_t,size_t,size_t,double)> observer_;
    struct State{std::atomic<size_t> pending{0},ready{0};};
    std::shared_ptr<State> state_=std::make_shared<State>();
    void Observe(const char *event,uint64_t offset,double wait=0)noexcept{try{if(observer_)observer_(event,offset,state_->pending.load(),state_->ready.load(),wait);}catch(...){}}
    void Fill() {
        while (next_ < end_ && pending_.size() < 4) {
            const auto offset=next_, length=std::min<uint64_t>(8*1024*1024,end_-next_);
            next_+=length;
            ++state_->pending;
            pending_.push_back(std::async(std::launch::async,[fetch=fetch_,offset,length,state=state_,observer=observer_]{
                const auto started=std::chrono::steady_clock::now();
                try{auto bytes=fetch(offset,length);++state->ready;try{if(observer)observer("prefetch_segment_ready",offset,state->pending.load(),state->ready.load(),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());}catch(...){}return bytes;}
                catch(...){try{if(observer)observer("prefetch_segment_failed",offset,state->pending.load(),state->ready.load(),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());}catch(...){}throw;}
            }));
        }
    }
  public:
    ParallelReader(uint64_t start,uint64_t end,Fetch fetch,std::function<void(const char*,uint64_t,size_t,size_t,double)> observer={}):fetch_(std::move(fetch)),next_(start),end_(end),ordered_(start),observer_(std::move(observer)){Fill();Observe("prefetch_start",start);}
    size_t Read(char *destination,size_t length) {
        if(consumed_==current_.size()) {
            if(pending_.empty())return 0;
            ordered_+=current_.size();const auto started=std::chrono::steady_clock::now();Observe("ordered_segment_wait",ordered_);
            current_=pending_.front().get();pending_.pop_front();--state_->pending;--state_->ready;consumed_=0;Fill();
            Observe("ordered_segment_ready",ordered_,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
            if(current_.empty())throw std::runtime_error("empty segment");
        }
        const auto count=std::min(length,current_.size()-consumed_);
        std::copy_n(current_.data()+consumed_,count,destination);consumed_+=count;return count;
    }
};
}
