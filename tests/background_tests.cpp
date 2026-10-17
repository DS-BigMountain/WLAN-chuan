// Directed expiry tests advance timestamps without waiting or reading user files.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "core.h"
#undef private
#include <iostream>
#include <stdexcept>
using namespace chuan;
static void Check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int wmain(){try{
    Config cfg;cfg.id=L"background-test";cfg.name=L"Test";Engine engine(cfg);const auto now=GetTickCount64();
    const auto makeJob=[&](const wchar_t *name,const wchar_t *peer,uint64_t total,bool receiving){auto job=std::make_shared<Engine::Job>();job->view.id=engine.jobs_.size()+1;job->view.name=name;job->view.peer=peer;job->view.total=total;job->view.receiving=receiving;engine.jobs_.push_back(job);return job;};
    auto &sender=engine.browsers_["sender"];sender.id=L"web:sender";sender.tabs["tab"]=now-65000;sender.openTabs.insert("tab");
    Peer peer;peer.id=sender.id;peer.name=L"A1";peer.browser=true;peer.platform="desktop-web";peer.seen=now-65000;engine.peers_.push_back(peer);
    Peer native;native.id=L"native";native.platform="windows";native.seen=now-10000;engine.peers_.push_back(native);
    auto job=makeJob(L"HT.zip",L"A1",19685133076,true);job->webClient="sender";job->webTab="tab";job->announced=true;job->view.started=now-600000;job->view.activity=L"等待发送端打包 ZIP";
    auto &recipient=engine.browsers_["recipient"];recipient.id=L"web:recipient";recipient.tabs["receive"]=now-600000;recipient.openTabs.insert("receive");
    auto waiting=makeJob(L"Waiting.zip",L"Sender",10,true);waiting->recipient="recipient";waiting->view.state=State::Waiting;waiting->view.awaitingApproval=true;waiting->view.requestOnly=true;
    engine.GetSnapshot();Check(job->view.state==State::Queued&&job->announced,"65-second background gap preserves packing");Check(engine.peers_.size()==1,"browser grace is separate from native presence");Check(waiting->view.state==State::Waiting&&waiting->view.awaitingApproval,"background receiving page preserves consent prompt");
    sender.tabs["tab"]=now-600000;engine.peers_[0].seen=now-600000;engine.GetSnapshot();Check(job->view.state==State::Queued&&job->announced&&job->browserPaused,"expired heartbeat pauses instead of failing");Check(job->view.activity==L"等待发送页面恢复","paused state has accurate label");Check(sender.openTabs.contains("tab"),"timeout does not claim page closure");
    sender.tabs["tab"]=GetTickCount64();recipient.tabs["receive"]=GetTickCount64();engine.GetSnapshot();Check(!job->browserPaused&&job->view.activity==L"等待发送端打包 ZIP","page resume restores packing state");Check(waiting->view.state==State::Waiting&&!waiting->browserPaused,"receiving page resumes without cancellation");
    sender.openTabs.insert("other");sender.tabs["other"]=GetTickCount64();auto other=makeJob(L"Other.zip",L"A1",10,true);other->webClient="sender";other->webTab="other";other->announced=true;
    sender.openTabs.erase("tab");sender.tabs.erase("tab");engine.GetSnapshot();Check(job->view.state==State::Failed&&job->view.error.find(L"已关闭")!=std::wstring::npos,"explicit source tab closure fails only its task");Check(other->view.state==State::Queued&&other->announced,"another browser tab remains valid");
    recipient.openTabs.clear();recipient.tabs.clear();engine.GetSnapshot();Check(waiting->view.state==State::Cancelled,"explicit receiving page closure cancels consent");
    std::vector<std::shared_ptr<Engine::Job>> approvals;
    for(int i=0;i<3;++i){auto pending=makeJob(L"Pending.txt",L"A1",10,true);pending->view.requestOnly=true;pending->view.awaitingApproval=true;pending->view.state=State::Waiting;pending->webClient="sender";pending->webTab="other";pending->asyncOfferKey="async-"+std::to_string(i);pending->view.offerItems.push_back({L"Pending.txt",false,1,10});engine.batchApprovals_[pending->asyncOfferKey].items=pending->view.offerItems;approvals.push_back(pending);}
    engine.DecideItems(approvals[1]->view.id,"1");Check(approvals[1]->view.state==State::Completed&&engine.batchApprovals_[approvals[1]->asyncOfferKey].selection=="1","out-of-order asynchronous consent creates upload authorization");Check(approvals[0]->view.awaitingApproval&&approvals[2]->view.awaitingApproval,"other pending requests remain visible");
    engine.Cancel(approvals[0]->view.id);Check(approvals[0]->view.state==State::Cancelled&&!approvals[0]->view.awaitingApproval&&approvals[2]->view.awaitingApproval,"cancel only the selected asynchronous request");
    sender.tabs["other"]=now-600000;engine.GetSnapshot();Check(approvals[2]->view.state==State::Waiting&&approvals[2]->view.awaitingApproval,"background heartbeat pause preserves asynchronous consent");sender.openTabs.clear();engine.GetSnapshot();Check(approvals[2]->view.state==State::Cancelled&&!approvals[2]->view.awaitingApproval,"source page closure clears asynchronous consent");
    Check(PeerLabel(peer)==L"电脑 Web","desktop browser label");peer.platform="mobile-web";Check(PeerLabel(peer)==L"手机 Web","mobile browser label");Check(PeerLabel(native)==L"Win 客户端","Windows label");native.platform="android";Check(PeerLabel(native)==L"Android 端","Android label");
    std::cout<<"PASS Windows background heartbeat grace, paused queue/resume, explicit close, tab isolation, receiving consent and device labels\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
