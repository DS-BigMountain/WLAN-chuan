#pragma once
#include "core.h"
#include <algorithm>
#include <functional>

namespace chuan {
struct TransferRow : Transfer {
    bool folderRow = false, childRow = false, directoryRow = false;
    unsigned depth = 0;
    std::wstring treePath;
    std::wstring currentFile, currentAction, stageSummary;
    uint64_t currentDone=0,currentTotal=0,currentCount=0,transferringFiles=0;
    bool currentTransferring=false;
    std::string group;
    std::vector<uint64_t> members;
    uint64_t completedFiles = 0, failedFiles = 0, cancelledFiles = 0, skippedFiles = 0;
};
inline std::string TransferGroup(const Transfer &job) {
    return job.folderId.empty() ? std::string{} : job.folderId + (job.receiving ? ":R:" : ":S:") + Utf8(job.peer);
}
inline bool ProcessingFile(const Transfer &job) {
    return !job.removed&&job.state<State::Completed&&(job.state==State::Running||
        (!job.activity.empty()&&job.activity!=L"等待发送端打包 ZIP"&&job.activity!=L"等待重试数据"&&job.activity!=L"等待接收方确认"&&
         job.activity!=L"等待核对同名文件"&&job.activity!=L"等待打包文件"&&job.activity!=L"已就绪，等待传输"&&job.activity!=L"等待浏览器接收"&&job.activity!=L"等待发送页面恢复"&&job.activity!=L"等待接收页面恢复"));
}
inline std::wstring TransferAction(const Transfer &job) {
    if(job.state>=State::Completed)return TransferText(job);
    static const std::map<std::wstring,std::wstring> labels={
        {L"正在发送文件",L"发送文件"},{L"正在接收文件",L"接收文件"},
        {L"正在保存文件",L"写入磁盘"},{L"正在校验接收内容",L"校验接收数据"},
        {L"正在核对文件内容",L"核对同名文件"},{L"正在校验已有文件",L"核对同名文件"},
        {L"正在核对目标文件",L"等待接收端核对同名文件"},{L"等待发送端核对同名文件",L"发送端正在核对同名文件"},
        {L"准备接收文件",L"准备接收"},{L"正在检查目标目录",L"检查接收位置"},
        {L"正在打包 ZIP",L"打包文件"},{L"等待发送端打包 ZIP",L"等待发送端开始打包"},
        {L"等待接收端确认",L"等待接收端响应"},{L"等待接收端保存确认",L"等待接收端保存完成"},
        {L"独立通道自动重试中",L"连接中断，正在重试"},{L"等待浏览器提交文件",L"等待浏览器保存确认"},
        {L"等待发送页面恢复",L"发送页面暂时离线"},{L"等待接收页面恢复",L"接收页面暂时离线"}
    };
    if(const auto found=labels.find(job.activity);found!=labels.end())return found->second;
    if(!job.activity.empty())return job.activity;
    if(job.awaitingApproval)return L"等待接收方确认";
    if(job.state==State::Running)return job.directory?L"创建目录":job.receiving?L"接收文件":L"发送文件";
    return job.receiving?L"等待发送端开始下一项":L"排队等待";
}
inline bool TransferringFile(const Transfer &job) {
    if(job.state!=State::Running||job.directory||job.paused)return false;
    const auto action=TransferAction(job);
    return job.activity.empty()||action==L"发送文件"||action==L"接收文件"||
        action==L"多连接并行发送"||action==L"多连接并行接收"||action==L"边打包边发送 ZIP";
}
struct FolderNode {
    std::wstring path, parent, name, error;
    uint64_t total = 0, done = 0, count = 0, completed = 0, failed = 0, cancelled = 0, active = 0, running = 0;
    double speed = 0;
    State state = State::Queued; bool directory = false, receiving = false;
};
inline std::vector<FolderNode> FolderTree(const std::vector<Transfer> &jobs) {
    std::map<std::wstring, FolderNode> nodes;
    for (const auto &job : jobs) if (!job.removed && !job.requestOnly) {
        fs::path current; std::vector<std::wstring> parts; for (const auto &part : fs::path(job.name)) parts.push_back(part.wstring());
        for (size_t i=0;i<parts.size();++i) {
            const auto parent=current.wstring(); current/=parts[i];auto &node=nodes[current.wstring()];
            node.path=current.wstring();node.parent=parent;node.name=parts[i];node.directory=node.directory||i+1<parts.size()||job.directory;node.receiving=job.receiving;
            node.total+=job.total;node.done+=std::min(job.done,job.total);node.count++;node.completed+=job.state==State::Completed;node.failed+=job.state==State::Failed;node.cancelled+=job.state==State::Cancelled;node.active+=job.state<State::Completed;node.running+=job.state==State::Running;if(job.state==State::Running)node.speed+=job.speed;
            if (!node.directory) {node.state=job.state;node.error=job.error;}
        }
    }
    std::vector<FolderNode> result;for(auto &[key,node]:nodes) result.push_back(std::move(node));return result;
}
inline std::vector<TransferRow> TransferRows(const std::vector<Transfer> &jobs, const std::set<std::string> &expanded) {
    std::vector<TransferRow> result;
    std::map<std::string, std::vector<const Transfer *>> groups;
    for (auto &job : jobs) if (!job.requestOnly && !job.folderId.empty()) groups[TransferGroup(job)].push_back(&job);
    std::set<std::string> visited;
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        if (it->requestOnly) continue;
        auto key = TransferGroup(*it);
        if (key.empty()) { if (!it->removed) { TransferRow row; static_cast<Transfer &>(row) = *it; result.push_back(row); } continue; }
        if (!visited.insert(key).second) continue;
        TransferRow row;
        static_cast<Transfer &>(row) = *it;
        row.folderRow = true; row.group = key; row.name = it->folderName.empty() ? fs::path(it->name).begin()->wstring() : it->folderName;
        row.total = 0; row.done = 0; row.speed = 0; row.savedPath.clear(); row.awaitingApproval = false;
        row.error.clear();row.skipped=false;row.paused=true;
        row.activity.clear();
        const Transfer *current = nullptr;
        const auto priority = [](const Transfer &job) {
            return !job.activity.empty() ? 4 : job.state == State::Running ? 3 : job.state == State::Waiting ? 2 : 1;
        };
        row.id = UINT64_MAX; bool active = false, running = false, waiting = false,browserReady=false;std::wstring idleAction;
        State failure = State::Completed;
        uint64_t probing=0,verifying=0,deferred=0,transferring=0,ready=0;
        for (auto *job : groups[key]) {
            if(job->state<State::Completed&&!job->removed){
                const auto action=TransferAction(*job);
                transferring+=TransferringFile(*job);
                probing+=action==L"检查接收位置";
                verifying+=action==L"核对同名文件"||action==L"发送端正在核对同名文件"||action==L"等待接收端核对同名文件"||action==L"打包文件";
                deferred+=job->activity==L"等待核对同名文件"||job->activity==L"等待打包文件";
                ready+=job->activity==L"已就绪，等待传输";
            }
            if(job->state<State::Completed){browserReady|=job->activity==L"等待浏览器接收";if(job->activity==L"等待发送页面恢复"||job->activity==L"等待接收页面恢复")idleAction=TransferAction(*job);}
            row.currentCount+=ProcessingFile(*job);
            row.awaitingApproval|=job->awaitingApproval;if(job->state<State::Completed)row.paused&=job->paused;
            if (ProcessingFile(*job) &&
                (!current || priority(*job) > priority(*current) ||
                 (priority(*job) == priority(*current) && current->directory && !job->directory))) current = job;
            row.id = std::min(row.id, job->id); if (!job->removed) row.members.push_back(job->id);
            row.total = std::max(row.total, job->folderTotal);
            row.done += std::min(job->done, job->total);
            row.folderCount = std::max(row.folderCount, job->folderCount);
            row.completedFiles += job->state == State::Completed;
            row.skippedFiles += job->skipped;row.folderStopped|=job->folderStopped;
            row.failedFiles += job->state == State::Failed; row.cancelledFiles += job->state == State::Cancelled;
            if (job->state == State::Failed && row.error.empty()) row.error = job->name + L"：" + job->error;
            active |= job->state < State::Completed; running |= job->state == State::Running;
            waiting |= job->state == State::Waiting;
            if (job->state == State::Running) row.speed += job->speed;
            if (job->state > failure) failure = job->state;
            if (!job->removed && row.savedPath.empty() && !job->savedPath.empty()) {
                row.savedPath = job->savedPath;
                auto relative = fs::path(job->name);
                for (auto p = ++relative.begin(); p != relative.end(); ++p) row.savedPath = row.savedPath.parent_path();
            }
        }
        row.id |= (1ull << 63);
        if(probing+verifying+deferred+transferring+ready>1)row.stageSummary=L"检查 "+std::to_wstring(probing)+L" · 核对 "+std::to_wstring(verifying)+L" · 传输 "+std::to_wstring(transferring)+L" · 待传 "+std::to_wstring(ready)+(deferred?L" · 待核对 "+std::to_wstring(deferred):L"");
        row.transferringFiles=transferring;
        row.done = std::min(row.done, row.total);
        if (row.failedFiles) failure = State::Failed;
        row.state = active ? (running ? State::Running : waiting ? State::Waiting : State::Queued) :
            failure != State::Completed ? failure : row.completedFiles >= row.folderCount ? State::Completed : State::Waiting;
        if(!active)row.paused=false;
        if(!active&&row.folderStopped&&row.completedFiles<row.folderCount&&row.state!=State::Failed){row.state=State::Cancelled;row.cancelledFiles=std::max(row.cancelledFiles,row.folderCount-row.completedFiles);}
        if(!current&&idleAction.empty()&&deferred)idleAction=L"等待核对同名文件";
        if (current) {
            row.currentFile = fs::path(current->name).lexically_relative(fs::path(row.name)).wstring();
            if (row.currentFile.empty() || row.currentFile == L".") row.currentFile = current->name;
            row.currentAction=TransferAction(*current);
            row.currentDone=current->done;row.currentTotal=current->total;
            row.currentTransferring=TransferringFile(*current);
            row.activity = row.currentAction;
        } else if (row.state < State::Completed) {
            row.currentAction=!idleAction.empty()?idleAction:browserReady?L"等待浏览器开始接收":row.awaitingApproval?L"等待接收方确认":row.receiving?L"等待发送端开始下一项":L"排队中，等待前一项完成";
            if(!row.receiving&&waiting&&!browserReady&&idleAction.empty())row.currentAction=L"等待接收方确认";
            row.activity=row.currentAction;
        }
        if (row.members.empty()) continue;
        result.push_back(row);
        if (!expanded.contains(key)) continue;
        std::vector<Transfer> members;std::map<std::wstring,const Transfer *> sourceFiles;for(const auto *job:groups[key]){members.push_back(*job);sourceFiles[job->name]=job;}
        const auto nodes=FolderTree(members);
        std::map<std::wstring,std::vector<const FolderNode *>> children;
        for(const auto &node:nodes)children[node.parent].push_back(&node);
        for(auto &[parent,list]:children)std::sort(list.begin(),list.end(),[](const auto *a,const auto *b){return a->directory!=b->directory?a->directory>b->directory:a->name<b->name;});
        std::function<void(const std::wstring &,unsigned)> append=[&](const auto &parent,unsigned depth){
            for(const auto *node:children[parent]) {
                TransferRow child;child.childRow=true;child.directoryRow=node->directory;child.group=key;child.depth=depth;child.treePath=node->path;
                child.name=node->name;child.total=node->total;child.done=node->done;child.speed=node->speed;child.receiving=row.receiving;
                child.completedFiles=node->completed;child.folderCount=node->count;child.failedFiles=node->failed;child.cancelledFiles=node->cancelled;
                child.state=node->directory?(node->active?(node->running?State::Running:State::Queued):node->failed?State::Failed:node->cancelled?State::Cancelled:State::Completed):node->state;
                child.error=node->error;
                if(node->directory){uint64_t hash=1469598103934665603ull;for(auto c:key+Utf8(node->path)){hash^=static_cast<unsigned char>(c);hash*=1099511628211ull;}child.id=(hash&0x3fffffffffffffffull)|0xc000000000000000ull;}
                else if(const auto file=sourceFiles.find(node->path);file!=sourceFiles.end()){child.id=file->second->id;child.savedPath=file->second->savedPath;child.canRetry=file->second->canRetry;child.activity=file->second->activity;}
                result.push_back(child);
                if(node->directory&&expanded.contains(key+"|"+Utf8(node->path)))append(node->path,depth+1);
            }
        };
        append(row.name,0);

    }
    return result;
}
} // namespace chuan
