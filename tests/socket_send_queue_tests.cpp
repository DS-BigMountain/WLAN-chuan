#include "socket_send_queue.h"
#include <future>
#include <iostream>
using namespace chuan;
static void Require(bool value, const char *message) { if(!value)throw std::runtime_error(message); }
struct SocketPair {
    SOCKET sender=INVALID_SOCKET,receiver=INVALID_SOCKET;
    SocketPair(){
        SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        Require(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"bind");
        int length=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length);
        Require(listen(listener,1)==0,"listen");sender=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        Require(connect(sender,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"connect");
        receiver=accept(listener,nullptr,nullptr);closesocket(listener);Require(receiver!=INVALID_SOCKET,"accept");
        int buffer=65536;setsockopt(sender,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer));
        DWORD timeout=5000;setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    }
    ~SocketPair(){if(sender!=INVALID_SOCKET)closesocket(sender);if(receiver!=INVALID_SOCKET)closesocket(receiver);}
};
static char Pattern(size_t offset){return static_cast<char>((offset*17+(offset>>12))&255);}
static void Fill(SocketSendQueue &queue,size_t offset,size_t count){
    auto &buffer=queue.Buffer();for(size_t i=0;i<count;++i)buffer[i]=Pattern(offset+i);queue.Submit(static_cast<DWORD>(count));
}
int main(){try{
    WSADATA data{};Require(WSAStartup(MAKEWORD(2,2),&data)==0,"Winsock");
    {
        SocketPair sockets;size_t completed=0;int error=0;
        const size_t size=5*SocketSendQueue::ChunkBytes+17;
        SocketSendQueue queue(sockets.sender,size,[]{return true;},[&](size_t bytes){completed+=bytes;},error);
        // No receiver drains the socket while these four buffers are posted.
        const auto start=GetTickCount64();
        for(size_t i=0;i<4;++i)Fill(queue,i*SocketSendQueue::ChunkBytes,SocketSendQueue::ChunkBytes);
        Require(GetTickCount64()-start<2000,"posting sends must not wait for a stalled reader");
        Require(completed==0,"posted bytes are not reported as completed");
        auto reader=std::async(std::launch::async,[&]{
            std::array<char,65536> bytes{};size_t received=0;bool equal=true;
            while(received<size){const int count=recv(sockets.receiver,bytes.data(),static_cast<int>(std::min(bytes.size(),size-received)),0);if(count<=0)return false;
                for(int i=0;i<count;++i)if(bytes[i]!=Pattern(received+i))equal=false;received+=count;}
            return equal;
        });
        for(size_t offset=4*SocketSendQueue::ChunkBytes;offset<size;offset+=SocketSendQueue::ChunkBytes)
            Fill(queue,offset,std::min(SocketSendQueue::ChunkBytes,size-offset));
        queue.Finish();Require(completed==size&&error==0&&reader.get(),"ordered exact bytes and final short block");
        std::cout<<"PASS four in-flight buffers without receiver progress, ordered reuse, exact tail and completion accounting\n";
    }
    {
        SocketPair sockets;bool active=true,cancelled=false;int error=0;const auto start=GetTickCount64();
        {
            SocketSendQueue queue(sockets.sender,4*SocketSendQueue::ChunkBytes,[&]{return active;},[](size_t){},error);
            for(size_t i=0;i<4;++i)Fill(queue,i*SocketSendQueue::ChunkBytes,SocketSendQueue::ChunkBytes);
            active=false;try{queue.Buffer();}catch(const std::runtime_error &failure){cancelled=std::string(failure.what())=="cancel";}
        }
        Require(cancelled&&GetTickCount64()-start<2000,"stalled sends cancel and release buffers promptly");
        std::cout<<"PASS stalled-reader cancellation drains overlapped operations before releasing memory\n";
    }
    {
        SocketPair sockets;int error=0;bool failed=false;
        {
            SocketSendQueue queue(sockets.sender,4*SocketSendQueue::ChunkBytes,[]{return true;},[](size_t){},error);
            for(size_t i=0;i<4;++i)Fill(queue,i*SocketSendQueue::ChunkBytes,SocketSendQueue::ChunkBytes);
            linger reset{1,0};setsockopt(sockets.receiver,SOL_SOCKET,SO_LINGER,reinterpret_cast<char*>(&reset),sizeof(reset));
            closesocket(sockets.receiver);sockets.receiver=INVALID_SOCKET;
            try{queue.Finish();}catch(const std::runtime_error&){failed=true;}
        }
        Require(failed&&error!=0,"receiver reset fails pending sends");
        std::cout<<"PASS peer reset terminates pending sends with a socket error\n";
    }
    {
        SocketPair sockets;int error=0;size_t completed=0;
        SocketSendQueue::TcpSample sample{};
        sample.valid=true;sample.minRttUs=2000;sample.rttUs=4000;
        sample.idealBacklogBytes=3*1024*1024;sample.cwndBytes=3*1024*1024;sample.bytesInFlight=256*1024;
        const size_t size=2*SocketSendQueue::ChunkBytes;
        SocketSendQueue queue(sockets.sender,size,[]{return true;},[&](size_t bytes){completed+=bytes;},error);
        queue.EnableAdaptive([&]{return sample;});
        Require(queue.TargetDepth()==4&&queue.ChunkSize()>=512*1024&&queue.TargetBacklogBytes()>=2*1024*1024,
                "high-speed low-latency link expands backlog and chunk size for peak throughput");
        sample.rttUs=90000;sample.idealBacklogBytes=512*1024;sample.cwndBytes=512*1024;sample.bytesInFlight=384*1024;
        auto reader=std::async(std::launch::async,[&]{
            std::array<char,65536> bytes{};size_t received=0;
            while(received<size){const int count=recv(sockets.receiver,bytes.data(),static_cast<int>(std::min(bytes.size(),size-received)),0);if(count<=0)return false;received+=count;}
            return true;
        });
        size_t sent=0;
        while(sent<size){
            const size_t step=std::min(queue.ChunkSize(),size-sent);
            Fill(queue,sent,step);sent+=step;
        }
        queue.Finish();
        Require(completed==size&&reader.get(),"adaptive send completes exact payload");
        Require(queue.QueueDelayUs()>=80000&&queue.TargetDepth()<=2&&queue.ChunkSize()<=256*1024&&queue.TargetBacklogBytes()<=512*1024,
                "bufferbloat RTT inflation shrinks in-flight backlog and chunk size");
        SocketPair secondSockets;int secondError=0;
        sample.rttUs=4000;sample.idealBacklogBytes=4*1024*1024;sample.cwndBytes=4*1024*1024;sample.bytesInFlight=64*1024;
        SocketSendQueue secondQueue(secondSockets.sender,size,[]{return true;},[](size_t){},secondError);
        secondQueue.EnableAdaptive([&]{return sample;});
        Require(secondQueue.TargetBacklogBytes()<=SocketSendQueue::MaxTotalBacklogBytes/2,
                "concurrent active queues share total in-flight backlog budget");
        std::cout<<"PASS adaptive BDP/RTT guard preserves high-throughput window and shrinks bufferbloat backlog\n";
    }
    WSACleanup();return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
