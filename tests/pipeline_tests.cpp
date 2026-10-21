#include "pipeline.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
int main(){
    std::promise<void> releaseProbes,releaseVerification,releasePayload;
    auto probeGate=releaseProbes.get_future().share(),verificationGate=releaseVerification.get_future().share(),payloadGate=releasePayload.get_future().share();
    std::atomic<unsigned> probing{0},verifying{0},transferring{0},completed{0},maxProbing{0},maxVerifying{0},maxTransferring{0},maxReady{0};
    const auto high=[](auto &target,unsigned value){unsigned old=target;while(old<value&&!target.compare_exchange_weak(old,value)){};};
    auto run=std::async(std::launch::async,[&]{chuan::RunStagedFilePipeline(80,[&](size_t index){
        high(maxProbing,++probing);probeGate.wait();--probing;return index<48?chuan::PipelineRoute::Verify:chuan::PipelineRoute::Ready;
    },[&](size_t){high(maxVerifying,++verifying);verificationGate.wait();--verifying;return false;},[&](size_t){
        high(maxTransferring,++transferring);payloadGate.wait();--transferring;++completed;
    },[&](const char *phase,size_t,size_t count){if(std::string(phase)=="ready")high(maxReady,static_cast<unsigned>(count));});});
    const auto until=[](auto condition){const auto deadline=std::chrono::steady_clock::now()+5s;while(!condition()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();return condition();};
    const bool probeCapacity=until([&]{return probing==15;});releaseProbes.set_value();
    const bool overlap=until([&]{return verifying==15&&transferring==4;});releasePayload.set_value();
    const bool independent=until([&]{return completed==32;})&&verifying==15;
    releaseVerification.set_value();run.get();
    if(!probeCapacity||!overlap||!independent||maxProbing!=15||maxVerifying!=15||maxTransferring!=4||maxReady>32)
        throw std::runtime_error("pipeline capacity, isolation or backpressure failed");
    std::cout<<"PASS 15 metadata tasks and 15 verifications; four payloads; bounded ready queue; all missing files finish ahead of blocked verification\n";
}
