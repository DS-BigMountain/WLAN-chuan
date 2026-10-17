#pragma once
#include "core.h"
#include <algorithm>
#include <functional>

namespace chuan {
struct TransferRow : Transfer {
    bool folderRow = false, childRow = false, directoryRow = false;
    unsigned depth = 0;
    std::wstring treePath;
    std::string group;
    std::vector<uint64_t> members;
    uint64_t completedFiles = 0, failedFiles = 0, cancelledFiles = 0;
};
inline std::string TransferGroup(const Transfer &job) {
    return job.folderId.empty() ? std::string{} : job.folderId + (job.receiving ? ":R:" : ":S:") + Utf8(job.peer);
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
            node.path=current.wstring();node.parent=parent;node.name=parts[i];node.directory=i+1<parts.size();node.receiving=job.receiving;
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
        row.error.clear();
        row.id = UINT64_MAX; bool active = false, running = false, waiting = false;
        State failure = State::Completed;
        for (auto *job : groups[key]) {
            row.id = std::min(row.id, job->id); if (!job->removed) row.members.push_back(job->id);
            row.total = std::max(row.total, job->folderTotal);
            row.done += std::min(job->done, job->total);
            row.folderCount = std::max(row.folderCount, job->folderCount);
            row.completedFiles += job->state == State::Completed;
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
        row.done = std::min(row.done, row.total);
        if (row.failedFiles) failure = State::Failed;
        row.state = active ? (running ? State::Running : waiting ? State::Waiting : State::Queued) :
            failure != State::Completed ? failure : row.completedFiles >= row.folderCount ? State::Completed : State::Waiting;
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
                else if(const auto file=sourceFiles.find(node->path);file!=sourceFiles.end()){child.id=file->second->id;child.savedPath=file->second->savedPath;}
                result.push_back(child);
                if(node->directory&&expanded.contains(key+"|"+Utf8(node->path)))append(node->path,depth+1);
            }
        };
        append(row.name,0);

    }
    return result;
}
} // namespace chuan
