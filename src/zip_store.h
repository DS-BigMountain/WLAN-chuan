#pragma once
#include "core.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <objbase.h>

// STORE ZIP64: metadata is planned before consent; source bytes are read only after consent.
namespace chuan::archive {
struct Entry {
    fs::path source;
    std::string name;
    uint64_t size=0, offset=0;
    uint32_t crc=0;
    fs::file_time_type modified{};
    bool directory=false;
};
struct Plan {
    fs::path source;
    std::wstring name;
    std::vector<Entry> entries;
    uint64_t size=0, total=0, count=0, centralOffset=0, centralSize=0;
    uint16_t time=0,date=0;
};
struct Artifact {
    fs::path path;
    ~Artifact(){std::error_code ec;fs::remove(path,ec);fs::remove(path.parent_path(),ec);}
};
inline Plan Prepare(const fs::path &root){
    Plan plan;plan.source=root;plan.name=root.filename().wstring()+L".zip";
    plan.time=0;plan.date=33; // Stable archive bytes allow a later send to resume.
    const auto add=[&](const fs::path &source,bool directory){
        auto relative=source==root?root.filename():root.filename()/source.lexically_relative(root);
        if(SanitizeRelative(relative.wstring()).empty())throw std::runtime_error("ZIP path is invalid");
        Entry entry;entry.source=source;entry.name=Utf8(relative.generic_wstring())+(directory?"/":"");entry.directory=directory;
        if(entry.name.size()>65535)throw std::runtime_error("ZIP path is too long: "+Utf8(source.wstring()));
        if(!directory){entry.size=fs::file_size(source);entry.modified=fs::last_write_time(source);if(entry.size>UINT64_MAX-plan.total)throw std::runtime_error("ZIP size overflow");plan.total+=entry.size;++plan.count;}
        plan.entries.push_back(std::move(entry));
    };
    add(root,true);
    for(const auto &entry:fs::recursive_directory_iterator(root)){
        if((GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0)throw std::runtime_error("ZIP does not follow links: "+Utf8(entry.path().wstring()));
        if(entry.is_directory())add(entry.path(),true);else if(entry.is_regular_file())add(entry.path(),false);else throw std::runtime_error("ZIP source is not a regular file: "+Utf8(entry.path().wstring()));
    }
    std::sort(plan.entries.begin(),plan.entries.end(),[](const auto &a,const auto &b){return a.name<b.name;});uint64_t offset=0;
    for(auto &entry:plan.entries){const uint64_t overhead=74+entry.name.size();if(entry.size>UINT64_MAX-overhead||entry.size+overhead>UINT64_MAX-offset)throw std::runtime_error("ZIP size overflow");entry.offset=offset;offset+=entry.size+overhead;if(overhead>UINT64_MAX-plan.centralSize)throw std::runtime_error("ZIP size overflow");plan.centralSize+=overhead;}
    plan.centralOffset=offset;if(plan.centralSize>UINT64_MAX-98||offset>static_cast<uint64_t>(INT64_MAX)-plan.centralSize-98)throw std::runtime_error("ZIP size overflow");plan.size=offset+plan.centralSize+98;return plan;
}
inline uint32_t Crc(const unsigned char *data,size_t size,uint32_t crc){
    static const auto tables=[] {std::array<uint32_t,2048> t{};for(unsigned n=0;n<256;n++){auto c=n;for(int i=0;i<8;i++)c=c&1?0xedb88320^(c>>1):c>>1;t[n]=c;}for(unsigned n=256;n<2048;n++)t[n]=(t[n-256]>>8)^t[t[n-256]&255];return t;}();
    size_t i=0;for(;i+8<=size;i+=8){crc^=uint32_t(data[i])|uint32_t(data[i+1])<<8|uint32_t(data[i+2])<<16|uint32_t(data[i+3])<<24;crc=tables[1792+(crc&255)]^tables[1536+(crc>>8&255)]^tables[1280+(crc>>16&255)]^tables[1024+(crc>>24)]^tables[768+data[i+4]]^tables[512+data[i+5]]^tables[256+data[i+6]]^tables[data[i+7]];}for(;i<size;i++)crc=(crc>>8)^tables[(crc^data[i])&255];return crc;
}
struct Record {
    std::vector<unsigned char> bytes;
    explicit Record(size_t size):bytes(size){}
    void Put(size_t at,uint64_t value,size_t width){for(size_t i=0;i<width;i++)bytes[at+i]=static_cast<unsigned char>(value>>(i*8));}
    void Name(size_t at,const std::string &name){std::copy(name.begin(),name.end(),bytes.begin()+at);}
};
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
inline void Emit(Plan plan,const std::function<void(const void*,size_t)> &sink,const std::function<void(uint64_t,const std::wstring &)> &progress,const std::function<bool()> &cancelled){
    const auto check=[&]{if(cancelled())throw std::runtime_error("ZIP streaming cancelled");};check();
    uint64_t written=0,done=0;std::vector<unsigned char> pending;pending.reserve(1024*1024);
    const auto flush=[&]{if(!pending.empty()){sink(pending.data(),pending.size());pending.clear();}};
    const auto write=[&](const void *data,size_t length){check();if(length>plan.size-written)throw std::runtime_error("ZIP size overflow");written+=length;const auto *bytes=static_cast<const unsigned char*>(data);while(length){const auto count=std::min<size_t>(length,1024*1024-pending.size());pending.insert(pending.end(),bytes,bytes+count);bytes+=count;length-=count;if(pending.size()==1024*1024)flush();}};
    const auto record=[&](const Record &r){write(r.bytes.data(),r.bytes.size());};std::vector<unsigned char> buffer(1024*1024);uint64_t tick=0;
    for(auto &entry:plan.entries){
        check();Record local(50+entry.name.size());local.Put(0,0x04034b50,4);local.Put(4,45,2);local.Put(6,0x808,2);local.Put(10,plan.time,2);local.Put(12,plan.date,2);local.Put(18,0xffffffff,4);local.Put(22,0xffffffff,4);local.Put(26,entry.name.size(),2);local.Put(28,20,2);local.Name(30,entry.name);const auto extra=30+entry.name.size();local.Put(extra,1,2);local.Put(extra+2,16,2);local.Put(extra+4,entry.size,8);local.Put(extra+12,entry.size,8);record(local);
        uint32_t crc=0xffffffff;
        if(!entry.directory){
            const auto sourceError=[&](const char *reason){return std::runtime_error("无法打包源文件："+Utf8(entry.source.wstring())+"；"+reason);};
            Handle source(CreateFileW(entry.source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));LARGE_INTEGER size{};std::error_code ec;const auto modified=fs::last_write_time(entry.source,ec);
            if(source.value==INVALID_HANDLE_VALUE||!GetFileSizeEx(source.value,&size)||static_cast<uint64_t>(size.QuadPart)!=entry.size||ec||modified!=entry.modified)throw sourceError("file is unavailable or changed");
            uint64_t read=0;while(read<entry.size){check();DWORD count=0;const auto want=static_cast<DWORD>(std::min<uint64_t>(buffer.size(),entry.size-read));if(!ReadFile(source.value,buffer.data(),want,&count,nullptr)||count!=want)throw sourceError("file read failed");crc=Crc(buffer.data(),count,crc);write(buffer.data(),count);read+=count;done+=count;if(GetTickCount64()-tick>=100){progress(done,Wide(entry.name));tick=GetTickCount64();}}
        }
        entry.crc=crc^0xffffffff;Record descriptor(24);descriptor.Put(0,0x08074b50,4);descriptor.Put(4,entry.crc,4);descriptor.Put(8,entry.size,8);descriptor.Put(16,entry.size,8);record(descriptor);
    }
    for(const auto &entry:plan.entries){Record central(74+entry.name.size());central.Put(0,0x02014b50,4);central.Put(4,45,2);central.Put(6,45,2);central.Put(8,0x808,2);central.Put(12,plan.time,2);central.Put(14,plan.date,2);central.Put(16,entry.crc,4);central.Put(20,0xffffffff,4);central.Put(24,0xffffffff,4);central.Put(28,entry.name.size(),2);central.Put(30,28,2);central.Put(38,entry.directory?16:0,4);central.Put(42,0xffffffff,4);central.Name(46,entry.name);auto extra=46+entry.name.size();central.Put(extra,1,2);central.Put(extra+2,24,2);central.Put(extra+4,entry.size,8);central.Put(extra+12,entry.size,8);central.Put(extra+20,entry.offset,8);record(central);}
    Record end(98);end.Put(0,0x06064b50,4);end.Put(4,44,8);end.Put(12,45,2);end.Put(14,45,2);end.Put(24,plan.entries.size(),8);end.Put(32,plan.entries.size(),8);end.Put(40,plan.centralSize,8);end.Put(48,plan.centralOffset,8);end.Put(56,0x07064b50,4);end.Put(64,plan.centralOffset+plan.centralSize,8);end.Put(72,1,4);end.Put(76,0x06054b50,4);end.Put(84,65535,2);end.Put(86,65535,2);end.Put(88,0xffffffff,4);end.Put(92,0xffffffff,4);record(end);
    if(written!=plan.size)throw std::runtime_error("ZIP length changed");flush();check();progress(plan.total,L"");
}
inline std::shared_ptr<Artifact> Build(Plan plan,const fs::path &cache,const std::function<void(uint64_t,const std::wstring &)> &progress,const std::function<bool()> &cancelled){
    const auto check=[&]{if(cancelled())throw std::runtime_error("ZIP packing cancelled");};check();
    fs::create_directories(cache);ULARGE_INTEGER space{};if(!GetDiskFreeSpaceExW(cache.c_str(),&space,nullptr,nullptr)||space.QuadPart<plan.size||space.QuadPart-plan.size<2*1024*1024)throw std::runtime_error("打包 ZIP 所需的磁盘空间不足");
    GUID guid{};CoCreateGuid(&guid);auto folder=cache/Wide(Hex(std::string(reinterpret_cast<char*>(&guid),sizeof(guid))));if(!fs::create_directory(folder))throw std::runtime_error("ZIP cache directory cannot be created");
    auto artifact=std::make_shared<Artifact>();artifact->path=folder/plan.name;
    Handle output(CreateFileW(artifact->path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(output.value==INVALID_HANDLE_VALUE)throw std::runtime_error("ZIP output cannot be created");
    Emit(std::move(plan),[&](const void *data,size_t length){DWORD count=0;if(!WriteFile(output.value,data,static_cast<DWORD>(length),&count,nullptr)||count!=length)throw std::runtime_error("ZIP write failed");},progress,cancelled);
    if(!FlushFileBuffers(output.value))throw std::runtime_error("ZIP flush failed");return artifact;
}
}
