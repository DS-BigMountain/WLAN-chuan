#include "core.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace chuan;
static void Require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
static unsigned short Port(int type){SOCKET s=socket(AF_INET,type,type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);Require(bind(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"port bind");int size=sizeof(a);getsockname(s,reinterpret_cast<sockaddr*>(&a),&size);closesocket(s);return ntohs(a.sin_port);}
template<class Check>void Until(Check check,const char *message){const auto end=GetTickCount64()+20000;while(!check()){if(GetTickCount64()>end)throw std::runtime_error(message);Sleep(2);}}
static Transfer Active(Engine &engine){for(const auto &job:engine.GetSnapshot().transfers)if(!job.requestOnly&&job.state<State::Completed)return job;return {};}
static void Equal(const fs::path &a,const fs::path &b){Require(fs::file_size(a)==fs::file_size(b),"file size");std::ifstream left(a,std::ios::binary),right(b,std::ios::binary);std::vector<char> x(1024*1024),y(x.size());while(left){left.read(x.data(),x.size());right.read(y.data(),y.size());Require(left.gcount()==right.gcount()&&std::equal(x.begin(),x.begin()+left.gcount(),y.begin()),"file bytes");}}
int wmain(int argc,wchar_t **argv){try{
    Require(argc==2,"isolated directory required");WSADATA sockets{};WSAStartup(MAKEWORD(2,2),&sockets);const auto root=fs::absolute(argv[1])/std::to_wstring(GetCurrentProcessId());fs::create_directories(root);
    Config receiving,sending;receiving.id=L"resume-receiver";receiving.name=L"receiver";receiving.dataDir=root/L"receiver-config";receiving.receiveDir=root/L"received";receiving.port=Port(SOCK_STREAM);receiving.discoveryPort=Port(SOCK_DGRAM);receiving.autoReceive=false;
    sending.id=L"resume-sender";sending.name=L"sender";sending.dataDir=root/L"sender-config";sending.receiveDir=root/L"sender-received";sending.port=Port(SOCK_STREAM);sending.discoveryPort=Port(SOCK_DGRAM);
    const auto source=root/L"large.bin";{std::ofstream file(source,std::ios::binary);std::vector<char> bytes(1024*1024);for(size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<char>(i*17+(i>>12));for(int i=0;i<64;++i)file.write(bytes.data(),bytes.size());}
    auto receiver=std::make_unique<Engine>(receiving);Engine sender(sending);std::wstring error;Require(receiver->Start(error)&&sender.Start(error),"start");Require(sender.AddPeer("127.0.0.1:"+std::to_string(receiving.port),error),"connect");const auto peer=sender.GetSnapshot().peers.front();sender.SendFiles(peer,{source});uint64_t request=0;
    Until([&]{for(const auto &job:receiver->GetSnapshot().transfers)if(job.requestOnly&&job.awaitingApproval){request=job.id;return true;}return false;},"approval request");receiver->DecideItems(request,"1");
    Require(Active(*receiver).id!=0,"accepted item disappeared before its manifest");
    Until([&]{return Active(*receiver).done>4*1024*1024;},"initial data");const auto incoming=Active(*receiver);receiver->Pause(incoming.id,true);
    Until([&]{return Active(sender).paused;},"sender did not observe receive pause");Sleep(200);const auto saved=Active(*receiver).done;Sleep(400);Require(Active(*receiver).done==saved,"paused data advanced");receiver->Pause(incoming.id,false);Require(sender.WaitForIdle(std::chrono::seconds(20)),"resume timeout");Equal(source,receiving.receiveDir/L"large.bin");
    std::cout<<"PASS Windows approval handoff stays visible; receive pause propagates; exact resumed content\n";
    fs::remove(receiving.receiveDir/L"large.bin");receiving.autoReceive=true;Require(receiver->UpdateConfig(receiving,error),"automatic receive");sender.SendFiles(peer,{source});Until([&]{return Active(*receiver).done>4*1024*1024;},"restart initial data");const auto outgoing=Active(sender);sender.Pause(outgoing.id,true);Sleep(500);receiver->Stop();receiver=std::make_unique<Engine>(receiving);Require(receiver->Start(error),"receiver restart");sender.Cancel(outgoing.id);Require(sender.AddPeer("127.0.0.1:"+std::to_string(receiving.port),error),"reconnect after restart");sender.SendFiles(peer,{source});Require(sender.WaitForIdle(std::chrono::seconds(20)),"restart resume timeout");Equal(source,receiving.receiveDir/L"large.bin");
    const auto log=receiver->ExportDiagnostics();std::ifstream events(log);std::string line;bool reused=false;while(std::getline(events,line)){const auto at=line.find("\"resume_offset\":");if(at!=std::string::npos&&line[at+16]!='0')reused=true;}Require(reused,"restart did not reuse a saved prefix");
    std::cout<<"PASS Windows restart and same-file reselect reuse persistent partial data\n";sender.Stop();receiver->Stop();return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
