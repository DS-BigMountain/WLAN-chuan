// Bounded browser chunks feed one authenticated upload connection. No archive
// is assembled in the browser; native destinations receive bytes immediately.
void Engine::WebArchive(SOCKET s,const std::map<std::string,std::string> &h,const std::string &ip,const std::string &path){
    uint64_t length=0;
    if(!Number(Get(h,"content-length"),length)||!Get(h,"transfer-encoding").empty()){Reply(s,400,"Bad Request");return;}
    const auto client=Get(h,"x-chuan-client"),tab=Get(h,"x-chuan-tab");
    if(path=="/web/api/archive/open"){
        uint64_t total=0,item=0,offset=0;if(!Get(h,"x-chuan-offset").empty()&&!Number(Get(h,"x-chuan-offset"),offset)){Reply(s,400,"Bad Request");return;}const auto name=SanitizeRelative(Wide(Unhex(Get(h,"x-chuan-path"))));
        const auto target=Wide(Unhex(Get(h,"x-chuan-target")));
        if(length||!WebKey(Get(h,"x-chuan-upload"))||!Number(Get(h,"x-chuan-size"),total)||total>9007199254740959ull||offset>total||!Number(Get(h,"x-chuan-item"),item)||!name.ends_with(L".zip")||!Get(h,"x-chuan-folder").empty()){Reply(s,400,"Bad Request");return;}
        Config cfg;Peer peer;bool remote=false;
        {std::lock_guard lock(mutex_);cfg=config_;
            for(auto it=webArchiveStreams_.begin();it!=webArchiveStreams_.end();){if(Now()-it->second->touched>45000){shutdown(it->second->socket,SD_BOTH);it=webArchiveStreams_.erase(it);}else ++it;}
            size_t owned=0;for(const auto &[key,stream]:webArchiveStreams_)owned+=stream->client==client;
            const auto found=batchApprovals_.find("web:"+ip+":"+client+":"+Get(h,"x-chuan-batch"));
            if(webArchiveStreams_.size()>=16||owned>=4){Reply(s,503,"Service Unavailable");return;}
            if(found==batchApprovals_.end()||item>=found->second.items.size()){Reply(s,403,"Forbidden");return;}
            const auto &batch=found->second;const auto &entry=batch.items[item];
            if(item>=batch.selection.size()||batch.selection[item]!='1'||batch.cancelledItems.contains(item)||batch.target!=Get(h,"x-chuan-target")||entry.folder||entry.name!=name||entry.total!=total){Reply(s,403,"Forbidden");return;}
            if(!target.empty()&&target!=cfg.id){const auto foundPeer=std::find_if(peers_.begin(),peers_.end(),[&](const auto &p){return p.id==target;});if(foundPeer==peers_.end()){Reply(s,409,"Conflict");return;}peer=*foundPeer;remote=!peer.browser||!peer.owner.empty();}
        }
        if(cfg.maxTransferBytes&&total>cfg.maxTransferBytes){Reply(s,413,"Content Too Large");return;}
        auto stream=std::make_shared<WebArchiveStream>();stream->client=client;stream->tab=tab;stream->total=total-offset+32;stream->offset=offset;stream->direct=remote;stream->touched=Now();stream->close=[this](SOCKET socket){ReleaseSocket(socket);};
        if(remote){
            std::shared_ptr<Job> job;
            {std::lock_guard lock(mutex_);for(const auto &previous:jobs_)if(previous->webClient==client&&previous->webUpload==Get(h,"x-chuan-upload")&&!previous->view.receiving){if(previous->view.state<State::Completed){Reply(s,409,"Conflict");return;}job=previous;break;}}
            if(job){std::string ignored;BatchCommand(peer,"/api/retry-upload/"+Get(h,"x-chuan-upload"),Get(h,"x-chuan-batch"),"",ignored,{});}
            if(!job)job=NewJob(name,peer.name,total,false);stream->job=job;job->cancel=false;Progress(job,0);SetState(job,State::Waiting);
            {std::lock_guard lock(mutex_);job->webClient=client;job->webTab=tab;job->webUpload=Get(h,"x-chuan-upload");job->batchId=Get(h,"x-chuan-batch");job->batchItem=static_cast<size_t>(item);job->targetId=peer.id;job->senderName=Wide(Unhex(Get(h,"x-chuan-sender")));job->view.activity=L"边打包边转发 ZIP";TraceJobLocked(job);}
            stream->socket=Connect(peer.gateway.empty()?peer.ip:peer.gateway,peer.port);if(stream->socket==INVALID_SOCKET){SetState(job,State::Failed,L"无法连接接收端");Reply(s,502,"Bad Gateway");return;}
            TrackSocket(stream->socket,job);SetTimeout(stream->socket,150000,30000);
            const auto query="POST /api/upload HTTP/1.1\r\nHost: "+peer.ip+":"+std::to_string(peer.port)+"\r\nConnection: close\r\nExpect: 100-continue\r\nX-Chuan-Id: "+Hex(Utf8(cfg.id))+"\r\nX-Chuan-Server: "+Utf8(peer.owner.empty()?peer.id:peer.owner)+"\r\nX-Chuan-Port: "+std::to_string(boundPort_)+"\r\nX-Chuan-Platform: windows\r\nX-Chuan-Sender: "+Hex(Utf8(cfg.name))+"\r\nX-Chuan-Display-Sender: "+Get(h,"x-chuan-sender")+"\r\nX-Chuan-Path: "+Get(h,"x-chuan-path")+"\r\nX-Chuan-Size: "+std::to_string(total)+"\r\nX-Chuan-Batch: "+job->batchId+"\r\nX-Chuan-Item: "+std::to_string(item)+"\r\nX-Chuan-Upload: "+job->webUpload+"\r\nX-Chuan-Recipient: "+(peer.browser?Hex(Utf8(peer.id)):"")+"\r\nX-Chuan-Resume-Key: "+Get(h,"x-chuan-resume-key")+"\r\nX-Chuan-Offset: "+std::to_string(offset)+"\r\nX-Chuan-Prefix: "+Get(h,"x-chuan-prefix")+"\r\nContent-Length: "+std::to_string(stream->total)+"\r\n\r\n";
            std::string reply;std::map<std::string,std::string> fields;
            if(!SendAll(stream->socket,query)||!([&]{do{fields.clear();if(!ReadHeader(stream->socket,reply,fields))return false;}while(reply.starts_with("HTTP/1.1 102 "));return reply.starts_with("HTTP/1.1 100 ");})()){std::string reason;uint64_t count=0;if(Number(Get(fields,"content-length"),count)&&count<=4096){reason.resize(static_cast<size_t>(count));ReadExact(stream->socket,reason.data(),reason.size());}SetState(job,State::Failed,Wide(reason.empty()?"接收端未允许 ZIP 传输":reason));Reply(s,409,"Conflict",reason);return;}SetState(job,State::Running);
        }else{
            SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);int size=sizeof(address);
            if(listener==INVALID_SOCKET||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,1)||getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)){if(listener!=INVALID_SOCKET)closesocket(listener);Reply(s,503,"Service Unavailable");return;}
            SOCKET writer=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if(writer==INVALID_SOCKET||connect(writer,reinterpret_cast<sockaddr*>(&address),sizeof(address))){closesocket(listener);if(writer!=INVALID_SOCKET)closesocket(writer);Reply(s,503,"Service Unavailable");return;}
            SOCKET reader=accept(listener,nullptr,nullptr);closesocket(listener);stream->socket=writer;TrackSocket(writer);if(reader==INVALID_SOCKET){Reply(s,503,"Service Unavailable");return;}TrackSocket(reader);SetTimeout(reader,30000,30000);SetTimeout(writer,150000,30000);
            auto headers=h;headers["content-length"]=std::to_string(stream->total);headers["x-chuan-checksum"]="sha256";
            if(!Launch([this,reader,headers,ip,stream]{try{ReceiveUpload(reader,headers,ip,true);}catch(...){}ReleaseSocket(reader);})){ReleaseSocket(reader);Reply(s,503,"Service Unavailable");return;}
        }
        GUID guid{};CoCreateGuid(&guid);const auto key=Hex(std::string(reinterpret_cast<char*>(&guid),sizeof(guid)));
        {std::lock_guard lock(mutex_);webArchiveStreams_[key]=stream;}
        diagnostic_->Write("\"event\":\"web_archive_stream_open\",\"client_id\":"+Json(client)+",\"upload_id\":"+Json(Get(h,"x-chuan-upload"))+",\"direct\":"+(remote?"true":"false"));
        Reply(s,200,"OK","{\"stream\":"+Json(key)+",\"forwarded\":"+(remote?"true":"false")+",\"id\":"+Json(stream->job?std::to_string(stream->job->view.id):"")+"}","application/json");return;
    }
    const auto split=path.find_last_of('/');const auto key=path.substr(split+1);std::shared_ptr<WebArchiveStream> stream;
    {std::lock_guard lock(mutex_);auto found=webArchiveStreams_.find(key);if(found==webArchiveStreams_.end()||found->second->client!=client||found->second->tab!=tab){Reply(s,404,"Not Found");return;}stream=found->second;}
    if(path.starts_with("/web/api/archive/abort/")){
        if(length){Reply(s,400,"Bad Request");return;}shutdown(stream->socket,SD_BOTH);if(stream->job){const bool paused=Get(h,"x-chuan-paused")=="1",failed=paused||Get(h,"x-chuan-abort-failed")=="1";if(paused)Pause(stream->job->view.id,true);stream->job->cancel=!failed;auto error=Wide(Unhex(Get(h,"x-chuan-error")));if(error.size()>1024)error=L"ZIP 流中断";SetState(stream->job,failed?State::Failed:State::Cancelled,error);}{std::lock_guard lock(mutex_);webArchiveStreams_.erase(key);}Reply(s,200,"OK");return;
    }
    if(!path.starts_with("/web/api/archive/chunk/")){Reply(s,404,"Not Found");return;}
    std::unique_lock guard(stream->mutex);uint64_t offset=0;
    if(!Number(Get(h,"x-chuan-offset"),offset)||offset!=stream->received||!length||length>1024*1024||length>stream->total-stream->received){Reply(s,409,"Conflict");return;}
    std::vector<char> bytes(static_cast<size_t>(length));bool ok=ReadExact(s,bytes.data(),bytes.size())&&SendAll(stream->socket,bytes.data(),bytes.size());stream->touched=Now();
    if(ok){stream->received+=length;if(stream->job)Progress(stream->job,stream->offset+std::min(stream->received,stream->total-32));}
    if(ok&&stream->received<stream->total){Reply(s,202,"Accepted","{}","application/json");return;}
    std::string reply,body;std::map<std::string,std::string> fields;
    if(ok){do{fields.clear();ok=ReadHeader(stream->socket,reply,fields);}while(ok&&reply.starts_with("HTTP/1.1 102 "));uint64_t count=0;if(ok&&Number(Get(fields,"content-length"),count)&&count<=131072){body.resize(static_cast<size_t>(count));ok=ReadExact(stream->socket,body.data(),body.size());}}
    const bool complete=ok&&reply.starts_with("HTTP/1.1 201 ");
    if(!complete&&stream->job&&!stream->job->cancel){Peer peer;bool found=false;{std::lock_guard lock(mutex_);for(const auto &candidate:peers_)if(candidate.id==stream->job->targetId){peer=candidate;found=true;break;}}if(found){std::string state;if(BatchCommand(peer,"/api/upload-status/"+stream->job->webUpload,stream->job->batchId,"",state,{})==200&&state=="4")stream->job->cancel=true;}}
    if(stream->job){SetState(stream->job,complete?State::Completed:stream->job->cancel?State::Cancelled:State::Failed,complete?L"":L"接收端未确认 ZIP 保存");if(complete)body="{\"id\":"+Json(std::to_string(stream->job->view.id))+",\"forwarded\":true}";}
    if(!complete)shutdown(stream->socket,SD_BOTH);
    {std::lock_guard lock(mutex_);webArchiveStreams_.erase(key);}
    Reply(s,complete?201:stream->job&&stream->job->cancel?410:422,complete?"Created":"Unprocessable Content",complete?body:Utf8(L"ZIP 流式上传未完成，接收端未确认保存"),complete?"application/json":"text/plain");
}
