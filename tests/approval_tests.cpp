// Focused native approval and retry checks. Not part of the full test suite.
#include "core.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace chuan;
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static unsigned short Port(int kind) {
    SOCKET s = socket(AF_INET, kind, kind == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(s, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "allocate port");
    int length = sizeof(address); getsockname(s, reinterpret_cast<sockaddr *>(&address), &length);
    closesocket(s); return ntohs(address.sin_port);
}
static bool Until(const std::function<bool()> &fn) { for (int i=0;i<240;++i) { if (fn()) return true; Sleep(25); } return false; }
static Transfer Prompt(Engine &engine, uint64_t after=0) {
    for (const auto &job : engine.GetSnapshot().transfers) if (job.id>after && job.awaitingApproval) return job;
    return {};
}
static Config Settings(const fs::path &root, const wchar_t *name) {
    auto cfg=LoadConfig(root/name/L"config");cfg.receiveDir=root/name/L"received";cfg.name=name;cfg.automaticName=false;
    cfg.autoReceive=false;cfg.port=Port(SOCK_STREAM);cfg.discoveryPort=Port(SOCK_DGRAM);return cfg;
}
int wmain(int argc,wchar_t **argv) {
    try {
        Check(argc==2,"output directory required"); WSADATA data{};WSAStartup(MAKEWORD(2,2),&data);
        const auto root=fs::absolute(argv[1]);fs::create_directories(root/L"source"/L"资料"/L"子目录");
        const auto folder=root/L"source"/L"资料";
        std::ofstream(folder/L"a.txt")<<"approval retry";std::ofstream(folder/L"子目录"/L"b.txt")<<"nested content";
        auto senderConfig=Settings(root,L"Sender"),receiverConfig=Settings(root,L"Receiver");
        Engine sender(senderConfig),receiver(receiverConfig);std::wstring error;
        Check(sender.Start(error)&&receiver.Start(error),"start engines");
        Check(sender.AddPeer("127.0.0.1:"+std::to_string(receiverConfig.port),error),"connect receiver");
        auto peers=sender.GetSnapshot().peers;const auto peer=*std::find_if(peers.begin(),peers.end(),[&](const auto &p){return p.id==receiverConfig.id;});
        sender.SendFiles(peer,{folder});Check(Until([&]{return Prompt(receiver).id!=0;}),"folder approval visible");
        auto prompt=Prompt(receiver);receiver.DecideItems(prompt.id,"0");
        Check(prompt.offerItems.size()==1&&!prompt.offerItems[0].folder&&prompt.offerItems[0].name==L"资料.zip","folder offered as one ZIP file");
        Check(!fs::exists(senderConfig.dataDir/L"archive-send"),"no packing before consent");
        Check(Until([&]{auto s=sender.GetSnapshot();return s.transfers.size()==1&&std::all_of(s.transfers.begin(),s.transfers.end(),[](const auto &j){return j.state==State::Rejected;});}),"rejection reflected at sender");
        auto rejected=sender.GetSnapshot().transfers.front();Check(sender.RetryTransfer(rejected.id,error),"retry rejected folder");
        Check(Until([&]{return Prompt(receiver,prompt.id).id!=0;}),"retry gets a new approval");
        Check(!sender.RetryTransfer(rejected.id,error),"duplicate active retry prevented");
        prompt=Prompt(receiver,prompt.id);receiver.DecideItems(prompt.id,"1");
        Check(Until([&]{auto s=sender.GetSnapshot();return std::count_if(s.transfers.begin(),s.transfers.end(),[](const auto &j){return j.state==State::Completed;})==1;}),"retried ZIP completed");
        Check(fs::exists(receiverConfig.receiveDir/L"资料.zip"),"one ZIP saved");
        Check(!fs::exists(receiverConfig.receiveDir/L"资料"),"folder is not sent file by file");
        const auto received=receiver.GetSnapshot();Check(std::count_if(received.transfers.begin(),received.transfers.end(),[](const auto &j){return !j.requestOnly;})==1,"one receiver transfer record");
        sender.SendFiles(peer,{folder});Check(Until([&]{return Prompt(receiver,prompt.id).id!=0;}),"changed source approval visible");
        prompt=Prompt(receiver,prompt.id);std::ofstream(folder/L"子目录"/L"b.txt",std::ios::app)<<" changed after selection";receiver.DecideItems(prompt.id,"1");
        Check(Until([&]{for(const auto &job:sender.GetSnapshot().transfers)if(job.state==State::Failed&&job.error.find(L"b.txt")!=std::wstring::npos)return true;return false;}),"packing failure identifies source path");
        Check(Until([&]{for(const auto &job:receiver.GetSnapshot().transfers)if(job.state==State::Failed&&job.error.find(L"b.txt")!=std::wstring::npos)return true;return false;}),"packing failure synchronized to receiver");
        Check(!fs::exists(receiverConfig.receiveDir/L"资料 (2).zip"),"failed packing does not send a partial ZIP");
        const auto single=root/L"source"/L"single.txt";std::ofstream(single)<<"single retry";
        sender.SendFiles(peer,{single});Check(Until([&]{return Prompt(receiver,prompt.id).id!=0;}),"single file also uses batch approval");
        prompt=Prompt(receiver,prompt.id);receiver.Stop();
        Check(Until([&]{for(const auto &j:sender.GetSnapshot().transfers)if(j.name==L"single.txt"&&j.state==State::Failed)return true;return false;}),"network closure becomes failure");
        Transfer failed;for(const auto &j:sender.GetSnapshot().transfers)if(j.name==L"single.txt")failed=j;
        Engine replacement(receiverConfig);Check(replacement.Start(error),"restart receiver");
        Check(sender.AddPeer("127.0.0.1:"+std::to_string(receiverConfig.port),error),"reconnect receiver");
        Check(sender.RetryTransfer(failed.id,error),"retry network failure");
        Check(Until([&]{return Prompt(replacement).id!=0;}),"network retry requires fresh consent");
        replacement.DecideItems(Prompt(replacement).id,"1");
        Check(Until([&]{return fs::exists(receiverConfig.receiveDir/L"single.txt");}),"single retry saved");
        fs::remove(single);Check(!sender.RetryTransfer(failed.id,error)&&!error.empty(),"missing source has actionable error");
        std::cout<<"PASS native folder rejection/retry, new consent, duplicate guard, network failure/retry, single-file approval and missing source\n";
        return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
