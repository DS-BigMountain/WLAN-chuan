// Segment access is bound to an existing transfer and the original source identity.
void Engine::ServeSegment(SOCKET s,const WebFile &source,uint64_t offset,uint64_t length,bool web) {
    const auto job=source.delivery;
    if(!job||!source.hasIdentity||!length||length>8*1024*1024||offset>source.size||length>source.size-offset){Reply(s,416,"Range Not Satisfiable");return;}
    if(job->segmentActive.fetch_add(1)>=8){--job->segmentActive;Reply(s,429,"Too Many Requests");return;}
    const auto release=std::shared_ptr<void>(nullptr,[job](void*){--job->segmentActive;});
    {
        std::lock_guard lock(mutex_);
        if(!running_||job->cancel||job->paused||(web&&!config_.shareSoftware)){Reply(s,409,"Conflict");return;}
        if(web&&job->view.state!=State::Running){
            if(job->segmentActive.load()!=1){Reply(s,409,"Conflict");return;}
            job->segmentDone=0;job->view.done=0;job->progressBase=0;job->view.started=Now();
            job->view.state=State::Running;job->view.activity=L"多连接并行发送";TraceJobLocked(job);
        }
    }
    WinFile file(CreateFileW(source.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    BY_HANDLE_FILE_INFORMATION actual{};LARGE_INTEGER size{};
    if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&actual)||!GetFileSizeEx(file.value,&size)||
        static_cast<uint64_t>(size.QuadPart)!=source.size||actual.dwVolumeSerialNumber!=source.identity.dwVolumeSerialNumber||
        actual.nFileIndexHigh!=source.identity.nFileIndexHigh||actual.nFileIndexLow!=source.identity.nFileIndexLow||
        CompareFileTime(&actual.ftLastWriteTime,&source.identity.ftLastWriteTime)!=0||
        (actual.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||(web&&(!SafeWebPath(source.path,source.root)||!WebHandleMatches(file.value,source.path)))){
        Reply(s,409,"Conflict",Utf8(L"源文件已变化"));return;
    }
    LARGE_INTEGER position{};position.QuadPart=offset;
    if(!SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN)){Reply(s,409,"Conflict");return;}
    TrackSocket(s,job);
    if(!SendAll(s,"HTTP/1.1 200 OK\r\nConnection: "+std::string(allowConnectionReuse?"keep-alive":"close")+"\r\nCache-Control: no-store\r\nContent-Type: application/octet-stream\r\nContent-Length: "+std::to_string(length+32)+"\r\n\r\n"))return;
    diagnostic::Transfer trace(*diagnostic_,s,job->view.id,length,"parallel_segment",
        ",\"offset\":"+std::to_string(offset)+",\"request_id\":"+Json(requestTraceId)+",\"batch_id\":"+Json(job->batchId)+",\"upload_id\":"+Json(job->webUpload)+",\"client_id\":"+Json(job->webClient)+",\"recipient_id\":"+Json(job->recipient));
    try{
        Hash hash;uint64_t loaded=0;
        SocketSendQueue sends(s,length,[&]{return running_&&!job->cancel&&!job->paused;},
            [&](size_t bytes){trace.done+=bytes;trace.Sample();},trace.socketError);
        sends.SetObserver([&](size_t pending,uint64_t bytes,uint64_t oldest){trace.Gauge("send_pending_operations",pending);trace.Gauge("send_pending_bytes",bytes);trace.Gauge("send_oldest_operation_ms",oldest);});
        while(loaded<length){
            auto &buffer=sends.Buffer();DWORD count=0;
            {auto time=trace.Stage("disk_io",trace.diskMs);if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(std::min<uint64_t>(buffer.size(),length-loaded)),&count,nullptr)||!count)throw std::runtime_error("read");}
            {auto time=trace.Stage("checksum",trace.hashMs);hash.Add(buffer.data(),count);}
            {auto time=trace.Stage("network_or_queue_wait",trace.networkMs);sends.Submit(count);}loaded+=count;
        }
        {auto time=trace.Stage("network_or_queue_wait",trace.networkMs);sends.Finish();}
        const auto digest=hash.Finish();if(!SendAll(s,digest.data(),digest.size()))throw std::runtime_error("send");
        const auto sent=job->segmentDone.fetch_add(length)+length;Progress(job,std::min<uint64_t>(source.size,sent));
        if(web&&sent>=source.size){std::lock_guard lock(mutex_);if(!job->cancel&&!job->paused){job->view.state=State::Waiting;job->view.activity=L"等待浏览器保存确认";TraceJobLocked(job);}}
        trace.result="segment_sent";
        reuseConnection=allowConnectionReuse;
    }catch(const std::exception &error){trace.failureStage=error.what();trace.result="failed";}
}

std::vector<char> Engine::FetchSegment(const std::string &ip,unsigned short port,const std::string &token,
                                     uint64_t offset,uint64_t length,const std::shared_ptr<Job> &job,const std::shared_ptr<SegmentConnections> &connections){
    const auto active=[&]{return running_&&!job->cancel&&!job->paused;};
    for(unsigned attempt=0;attempt<2;++attempt){
    const auto slot=connections->Acquire(active);auto &socket=connections->Socket(slot);const bool reused=socket!=INVALID_SOCKET;bool healthy=false;
    const auto release=std::shared_ptr<void>(nullptr,[connections,slot,&healthy](void*){connections->Release(slot,healthy);});
    try{
    if(socket==INVALID_SOCKET)socket=Connect(ip,port);
    if(socket==INVALID_SOCKET)throw std::runtime_error("parallel connect");
    TrackSocket(socket,job);if(!active())throw std::runtime_error("cancel");
    const auto requestId=job->batchId+"-"+std::to_string(offset)+"-"+std::to_string(attempt);
    const auto query="GET /api/segments/"+token+" HTTP/1.1\r\nHost: "+ip+":"+std::to_string(port)+
        "\r\nConnection: keep-alive\r\nX-Chuan-Trace: "+requestId+"\r\nX-Chuan-Offset: "+std::to_string(offset)+"\r\nX-Chuan-Length: "+std::to_string(length)+"\r\n\r\n";
    std::string first;std::map<std::string,std::string> headers;uint64_t size=0;
    if(!SendAll(socket,query)||!ReadHeader(socket,first,headers)||!first.starts_with("HTTP/1.1 200 ")||
        !Number(Get(headers,"content-length"),size)||size!=length+32)throw std::runtime_error("parallel response");
    std::vector<char> bytes(static_cast<size_t>(length));
    diagnostic::Transfer trace(*diagnostic_,socket,job->view.id,length,"parallel_receive_segment",",\"offset\":"+std::to_string(offset)+",\"request_id\":"+Json(requestId)+",\"connection_reused\":"+(reused?"true":"false")+",\"batch_id\":"+Json(job->batchId)+",\"client_id\":"+Json(job->webClient)+",\"recipient_id\":"+Json(job->recipient));
    std::array<unsigned char,32> expected{};
    {auto time=trace.Stage("network_or_queue_wait",trace.networkMs);
    if(!ReadExact(socket,bytes.data(),bytes.size())||!ReadExact(socket,expected.data(),expected.size())){trace.socketError=WSAGetLastError();trace.failureStage="receive";throw std::runtime_error("parallel receive");}}
    Hash hash;{auto hashing=trace.Stage("checksum",trace.hashMs);hash.Add(bytes.data(),bytes.size());if(hash.Finish()!=expected){trace.failureStage="checksum";throw std::runtime_error("parallel checksum");}}
    if(!running_||job->cancel||job->paused)throw std::runtime_error("cancel");
    trace.done=length;trace.result="segment_verified";healthy=Get(headers,"connection")=="keep-alive";
    return bytes;
    }catch(const std::runtime_error &error){if(attempt||!reused||!active()||std::string(error.what()).find("checksum")!=std::string::npos)throw;}
    }
    throw std::runtime_error("parallel connection");
}

void Engine::WebParallelUpload(SOCKET s,const std::string &path,const std::map<std::string,std::string> &headers){
    const auto client=Get(headers,"x-chuan-client");
    if(path=="/web/api/parallel/open"){
        uint64_t total=0,start=0;
        if(Get(headers,"content-length")!="0"||!WebKey(Get(headers,"x-chuan-upload"))||!WebKey(Get(headers,"x-chuan-batch"))||
           !Number(Get(headers,"x-chuan-size"),total)||!Number(Get(headers,"x-chuan-offset"),start)||start>=total||total>9007199254740991ull){Reply(s,400,"Bad Request");return;}
        auto channel=std::make_shared<ParallelUpload>();channel->client=client;channel->upload=Get(headers,"x-chuan-upload");channel->batch=Get(headers,"x-chuan-batch");channel->total=total;channel->start=channel->next=start;channel->created=Now();
        GUID guid{};CoCreateGuid(&guid);const auto ticket=Hex(std::string(reinterpret_cast<char*>(&guid),sizeof(guid)));
        {std::lock_guard lock(mutex_);
         std::erase_if(parallelUploads_,[&](const auto &entry){const auto &old=entry.second;std::lock_guard guard(old->mutex);if(!old->attached&&Now()-old->created>60000){old->closed=true;old->changed.notify_all();return true;}return false;});
         if(parallelUploads_.size()>=4){Reply(s,503,"Service Unavailable");return;}parallelUploads_[ticket]=channel;}
        Reply(s,200,"OK","{\"ticket\":"+Json(ticket)+"}","application/json");return;
    }
    const auto ticket=path.substr(path.find_last_of('/')+1);std::shared_ptr<ParallelUpload> channel;
    {std::lock_guard lock(mutex_);auto found=parallelUploads_.find(ticket);if(found!=parallelUploads_.end()&&found->second->client==client)channel=found->second;}
    if(!channel){Reply(s,404,"Not Found");return;}
    if(path.starts_with("/web/api/parallel/close/")){
        if(Get(headers,"content-length")!="0"){Reply(s,400,"Bad Request");return;}
        channel->Close();{std::lock_guard lock(mutex_);parallelUploads_.erase(ticket);}Reply(s,200,"OK");return;
    }
    if(!path.starts_with("/web/api/parallel/part/")){Reply(s,404,"Not Found");return;}
    uint64_t offset=0,length=0,body=0;
    if(!Number(Get(headers,"x-chuan-offset"),offset)||!Number(Get(headers,"x-chuan-length"),length)||!length||length>ParallelUpload::SegmentBytes||
       !Number(Get(headers,"content-length"),body)||body!=length+32||!Get(headers,"transfer-encoding").empty()){Reply(s,400,"Bad Request");return;}
    try{
        const auto reservedAt=diagnostic::Milliseconds();channel->Reserve(offset,length);
        diagnostic::Transfer trace(*diagnostic_,s,channel->transferId,length,"parallel_upload_segment",",\"offset\":"+std::to_string(offset)+",\"request_id\":"+Json(requestTraceId)+",\"client_id\":"+Json(channel->client)+",\"upload_id\":"+Json(channel->upload)+",\"reorder_wait_ms\":"+std::to_string(diagnostic::Milliseconds()-reservedAt));
        std::vector<char> bytes(static_cast<size_t>(length));std::array<unsigned char,32> expected{};
        {auto time=trace.Stage("network_or_queue_wait",trace.networkMs);if(!ReadExact(s,bytes.data(),bytes.size())||!ReadExact(s,expected.data(),expected.size())){trace.failureStage="receive";trace.socketError=WSAGetLastError();throw std::runtime_error("connection");}}
        Hash hash;{auto time=trace.Stage("checksum",trace.hashMs);hash.Add(bytes.data(),bytes.size());if(hash.Finish()!=expected){trace.failureStage="checksum";throw std::runtime_error("checksum");}}
        channel->Put(offset,std::move(bytes));ReplyReusable(s,201,"Created");
        trace.done=length;trace.result="segment_received";
    }catch(const std::exception&){channel->Close();Reply(s,422,"Unprocessable Content");}
}
