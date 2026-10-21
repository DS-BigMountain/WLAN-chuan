#include "core.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace chuan;
static void Require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
static unsigned short Port(int type){SOCKET s=socket(AF_INET,type,type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);Require(bind(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"port bind");int size=sizeof(a);getsockname(s,reinterpret_cast<sockaddr*>(&a),&size);closesocket(s);return ntohs(a.sin_port);}
static void Write(const fs::path &path,const char *data){fs::create_directories(path.parent_path());std::ofstream(path,std::ios::binary)<<data;}
int wmain(int argc,wchar_t **argv){
    try{
        Require(argc==2,"provide isolated fixture directory");WSADATA data{};WSAStartup(MAKEWORD(2,2),&data);
        const auto base=fs::absolute(argv[1])/std::to_wstring(GetCurrentProcessId());fs::create_directories(base);
        Config receive,send;receive.dataDir=base/L"receiver-config";receive.receiveDir=base/L"received";receive.port=Port(SOCK_STREAM);receive.discoveryPort=Port(SOCK_DGRAM);receive.id=L"receiver";receive.name=L"receiver";
        send.dataDir=base/L"sender-config";send.receiveDir=base/L"sender-received";send.port=Port(SOCK_STREAM);send.discoveryPort=Port(SOCK_DGRAM);send.id=L"sender";send.name=L"sender";
        const auto source=base/L"source"/L"folder",target=receive.receiveDir/L"folder";
        Write(source/L"existing.txt","same");Write(source/L"new.txt","new");fs::create_directories(source/L"empty");
        Write(target/L"existing.txt","same");Write(target/L"unrelated.txt","unrelated");
        Engine receiver(receive),sender(send);std::wstring error;Require(receiver.Start(error),"receiver startup");Require(sender.Start(error),"sender startup");
        Require(sender.AddPeer("127.0.0.1:"+std::to_string(receive.port),error),"connect");const auto peers=sender.GetSnapshot().peers;Require(!peers.empty(),"peer");sender.SendFiles(peers.front(),{source});Require(sender.WaitForIdle(std::chrono::seconds(20)),"copy timeout");
        auto snapshot=receiver.GetSnapshot();size_t skipped=0,dirs=0,files=0;
        for(const auto &job:snapshot.transfers)if(!job.requestOnly){Require(job.state==State::Completed,"copy failed");skipped+=job.skipped;dirs+=job.directory;files+=!job.directory;Require(receiver.DeleteTransfer(job.id,true,error),"delete record");}
        Require(skipped==1&&dirs==2&&files==2,"copy fixture incomplete");
        Require(fs::is_directory(target/L"empty")&&fs::exists(target/L"existing.txt")&&fs::exists(target/L"unrelated.txt"),"existing content removed");Require(!fs::exists(target/L"new.txt"),"received file retained");
        for(const auto &job:receiver.GetSnapshot().transfers)Require(job.requestOnly||job.removed,"record retained");sender.Stop();receiver.Stop();
        std::cout<<"PASS Windows: directory and skipped originals retained; only newly received file deleted; records removed\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
