#pragma once
#include "core.h"
#include <algorithm>

namespace chuan {
struct TransferRow : Transfer {
    bool folderRow = false, childRow = false;
    std::string group;
    std::vector<uint64_t> members;
    uint64_t completedFiles = 0;
};
inline std::string TransferGroup(const Transfer &job) {
    return job.folderId.empty() ? std::string{} : job.folderId + (job.receiving ? ":R:" : ":S:") + Utf8(job.peer);
}
inline std::vector<TransferRow> TransferRows(const std::vector<Transfer> &jobs, const std::set<std::string> & /*expanded*/) {
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
        row.folderRow = true; row.group = key; row.name = it->folderName;
        row.total = 0; row.done = 0; row.speed = 0; row.savedPath.clear(); row.awaitingApproval = false;
        row.id = UINT64_MAX; bool active = false, running = false, waiting = false;
        State failure = State::Completed;
        for (auto *job : groups[key]) {
            row.id = std::min(row.id, job->id); if (!job->removed) row.members.push_back(job->id);
            row.total = std::max(row.total, job->folderTotal);
            row.done += std::min(job->done, job->total);
            row.folderCount = std::max(row.folderCount, job->folderCount);
            row.completedFiles += job->state == State::Completed;
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
        row.state = active ? (running ? State::Running : waiting ? State::Waiting : State::Queued) :
            failure != State::Completed ? failure : row.completedFiles >= row.folderCount ? State::Completed : State::Waiting;
        if (row.members.empty()) continue;
        result.push_back(row);

    }
    return result;
}
} // namespace chuan
