#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

namespace chuan {
inline constexpr size_t FileProbeLimit=15, FileVerificationLimit=15;
inline constexpr size_t FileTransferLimit=4, FileReadyLimit=32;
enum class PipelineRoute { Done, Ready, Verify };
// Deferred entries are indices into the existing manifest, not file contents.
// Only ready payloads apply backpressure to metadata discovery.
template<class Probe, class Verify, class Transfer>
void RunStagedFilePipeline(size_t count, Probe probe, Verify verify, Transfer transfer,
                          std::function<void(const char*,size_t,size_t)> observe = {}) {
    if (!count) return;
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<size_t> deferred, ready;
    size_t probing=std::min(FileProbeLimit,count), verifying=std::min(FileVerificationLimit,count);
    std::vector<std::jthread> probes, verifiers, writers;
    const auto publish=[&](size_t index){
        size_t queued;
        {std::unique_lock lock(mutex);changed.wait(lock,[&]{return ready.size()<FileReadyLimit;});ready.push_back(index);queued=ready.size();}
        changed.notify_all();if(observe)observe("ready",index,queued);
    };
    for(size_t i=0;i<std::min(FileTransferLimit,count);++i)writers.emplace_back([&]{
        for(;;){
            size_t index,queued;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return !ready.empty()||(!probing&&!verifying);});if(ready.empty())return;index=ready.front();ready.pop_front();queued=ready.size();}
            changed.notify_all();if(observe)observe("dequeue",index,queued);transfer(index);
        }
    });
    for(size_t i=0,n=verifying;i<n;++i)verifiers.emplace_back([&]{
        for(;;){
            size_t index;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return !deferred.empty()||!probing;});if(deferred.empty())break;index=deferred.front();deferred.pop_front();}
            if(verify(index))publish(index);
        }
        {std::lock_guard lock(mutex);--verifying;}changed.notify_all();
    });
    for(size_t i=0,n=probing;i<n;++i)probes.emplace_back([&]{
        for(size_t index;(index=next.fetch_add(1))<count;){
            const auto result=probe(index);
            if(result==PipelineRoute::Ready)publish(index);
            else if(result==PipelineRoute::Verify){
                size_t queued;{std::lock_guard lock(mutex);deferred.push_back(index);queued=deferred.size();}
                changed.notify_all();if(observe)observe("verification_queued",index,queued);
            }
        }
        {std::lock_guard lock(mutex);--probing;}changed.notify_all();
    });
    for(auto &thread:probes)thread.join();
    for(auto &thread:verifiers)thread.join();
    for(auto &thread:writers)thread.join();
}
// Compatibility entry point for callers that have no metadata-only operation.
template<class Prepare, class Transfer>
void RunFilePipeline(size_t count, Prepare prepare, Transfer transfer,
                     std::function<void(const char*,size_t,size_t)> observe = {}) {
    RunStagedFilePipeline(count,[](size_t){return PipelineRoute::Verify;},prepare,transfer,observe);
}
}
