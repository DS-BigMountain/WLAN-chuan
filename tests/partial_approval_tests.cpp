#define wmain LegacyApprovalTest
#include "approval_tests.cpp"
#undef wmain
int wmain(int argc,wchar_t **argv){try{
    Check(argc==2,"output directory required");WSADATA data{};WSAStartup(MAKEWORD(2,2),&data);const auto root=fs::absolute(argv[1]);
    auto cfg=Settings(root,L"Sender"),received=Settings(root,L"Receiver");Engine sender(cfg),receiver(received);std::wstring error;Check(sender.Start(error)&&receiver.Start(error),"start engines");
    Check(sender.AddPeer("127.0.0.1:"+std::to_string(received.port),error),"connect receiver");auto peers=sender.GetSnapshot().peers;const auto peer=*std::find_if(peers.begin(),peers.end(),[&](const auto &p){return p.id==received.id;});
    const auto a=root/L"source"/L"first",b=root/L"source"/L"second",c=root/L"source"/L"keep.txt";fs::create_directories(a);fs::create_directories(b);std::ofstream(a/L"a.txt")<<"first";std::ofstream(b/L"b.txt")<<"second";std::ofstream(c)<<"keep";
    sender.SendFiles(peer,{a,b,c});Check(Until([&]{return Prompt(receiver).id!=0;}),"native approval arrives");const auto request=Prompt(receiver);Check(request.offerItems.size()==3,"three original items");
    receiver.DecideItems(request.id,"1--");Check(Until([&]{return fs::exists(received.receiveDir/L"first.zip");}),"first selected ZIP sent while other items await consent");
    auto pending=Prompt(receiver);Check(pending.id==request.id&&pending.offerSelection=="1--","unchecked native items stay pending in the same request");Check(!fs::exists(received.receiveDir/L"second.zip")&&!fs::exists(received.receiveDir/L"keep.txt"),"no unselected payload sent");
    receiver.DecideItems(request.id,"01-");Check(Until([&]{return fs::exists(received.receiveDir/L"second.zip");}),"second ZIP registration succeeds incrementally");Check(Prompt(receiver).offerSelection=="11-","later decisions do not revoke previous acceptance");
    Transfer waiting;for(const auto &job:sender.GetSnapshot().transfers)if(job.name==L"keep.txt")waiting=job;sender.Cancel(waiting.id);
    Check(Until([&]{return Prompt(receiver).id==0;}),"cancel one pending item removes its remaining prompt");Check(!fs::exists(received.receiveDir/L"keep.txt"),"canceled pending file is never sent");
    Check(Until([&]{auto s=sender.GetSnapshot();return std::count_if(s.transfers.begin(),s.transfers.end(),[](const auto &job){return job.state==State::Completed;})==2;}),"accepted ZIP jobs complete once each");
    std::cout<<"PASS native incremental consent, retained pending items, monotonic decisions, two incremental ZIP registrations and isolated cancellation\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
