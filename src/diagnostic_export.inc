    bool Export(const std::filesystem::path &target,const std::string &clientJson="",bool consume=true) noexcept {
        try{
            std::lock_guard exporting(exportMutex_);Flush();std::lock_guard io(ioMutex_);stream_.flush();
            struct Segment{std::filesystem::path path;std::string name;uint64_t start,end;};std::vector<Segment> files;
            const auto owner=clientJson.empty()?std::string("native"):clientJson;auto cursor=exportCursors_.contains(owner)?exportCursors_.at(owner):exportCursors_["__initial__"];
            for(const auto &entry:std::filesystem::directory_iterator(directory_))if(entry.is_regular_file()&&entry.path().filename().wstring().starts_with(L"transfer-")&&entry.path().extension()==L".jsonl"){
                const auto name=entry.path().filename().string();const auto size=entry.file_size();const auto previous=cursor.contains(name)?cursor.at(name):0;
                files.push_back({entry.path(),name,std::min<uint64_t>(previous,size),size});
            }
            std::sort(files.begin(),files.end(),[](const auto &a,const auto &b){return std::filesystem::last_write_time(a.path)>std::filesystem::last_write_time(b.path);});
            uint64_t bytes=0,omitted=0;size_t selected=0;std::set<std::string> selectedNames;
            for(const auto &file:files){cursor[file.name]=file.end;const auto count=file.end-file.start;if(!count)continue;if(bytes+count>256ull*1024*1024){omitted+=count;continue;}bytes+=count;++selected;selectedNames.insert(file.name);}
            std::ofstream out(target,std::ios::binary);if(!out)return false;
            out<<"{\"event\":\"export_manifest\",\"diagnostic_schema\":2,\"diagnostic_profile\":\"transport-detail\",\"platform\":\"windows\",\"incremental\":true,\"dropped_events\":"<<dropped_<<",\"storage_errors\":"<<ioErrors_<<",\"omitted_bytes\":"<<omitted<<",\"selected_segments\":"<<selected<<"}\n";
            uint64_t records=0;
            for(auto it=files.rbegin();it!=files.rend();++it){const auto &file=*it;if(!selectedNames.contains(file.name))continue;
                std::ifstream input(file.path,std::ios::binary);if(!input)throw std::runtime_error("log read");input.seekg(static_cast<std::streamoff>(file.start));std::string line;
                while(input&&static_cast<uint64_t>(input.tellg())<file.end&&std::getline(input,line)){
                    if(clientJson.empty()||line.find("\"event\":\"session_start\"")!=std::string::npos||line.find("\"event\":\"resources\"")!=std::string::npos||line.find("\"client_id\":"+clientJson)!=std::string::npos||line.find("\"recipient_id\":"+clientJson)!=std::string::npos){out<<line<<'\n';++records;}
                }
            }
            out<<"{\"event\":\"export_summary\",\"incremental\":true,\"exported_records\":"<<records<<",\"omitted_bytes\":"<<omitted<<"}\n";
            out.flush();if(!out)return false;
            if(!consume){pendingExports_[target.wstring()]={owner,std::move(cursor)};return true;}
            const bool existed=exportCursors_.contains(owner);const auto previous=exportCursors_[owner];exportCursors_[owner]=std::move(cursor);
            try{SaveCursors();}catch(...){if(existed)exportCursors_[owner]=previous;else exportCursors_.erase(owner);throw;}
            return true;
        }catch(...){return false;}
    }
    bool CommitExport(const std::filesystem::path &target)noexcept{
        try{std::lock_guard lock(exportMutex_);auto found=pendingExports_.find(target.wstring());if(found==pendingExports_.end())return false;
            const auto owner=found->second.first;auto previous=exportCursors_.contains(owner)?exportCursors_.at(owner):exportCursors_["__initial__"];auto cursor=previous;
            for(const auto &[name,offset]:found->second.second)cursor[name]=std::max(cursor[name],offset);
            exportCursors_[owner]=std::move(cursor);try{SaveCursors();}catch(...){exportCursors_[owner]=std::move(previous);throw;}pendingExports_.erase(found);return true;
        }catch(...){return false;}
    }
    void AbortExport(const std::filesystem::path &target)noexcept{try{std::lock_guard lock(exportMutex_);pendingExports_.erase(target.wstring());}catch(...){}}
    bool Clear() noexcept {
        try{
            std::lock_guard exporting(exportMutex_);Flush();std::lock_guard io(ioMutex_);
            {std::lock_guard lock(mutex_);++epoch_;completed_+=queue_.size();queue_.clear();}
            stream_.close();
            for(const auto &entry:std::filesystem::directory_iterator(directory_))if(entry.is_regular_file()&&entry.path().filename().wstring().starts_with(L"transfer-")&&entry.path().extension()==L".jsonl")std::filesystem::remove(entry.path());
            exportCursors_.clear();pendingExports_.clear();exportCursors_["__initial__"];SaveCursors();dropped_=0;ioErrors_=0;
            path_=directory_/(prefix_+"-"+std::to_string(++segment_)+".jsonl");stream_.clear();stream_.open(path_,std::ios::binary);bytes_=0;
            flushed_.notify_all();return stream_.good();
        }catch(...){++ioErrors_;try{std::lock_guard io(ioMutex_);if(!stream_.is_open()){stream_.clear();path_=directory_/(prefix_+"-"+std::to_string(++segment_)+".jsonl");stream_.open(path_,std::ios::binary);bytes_=0;}}catch(...){}return false;}
    }
