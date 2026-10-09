// Batch negotiation is completed before payload transfer. The consent ledger is
// bound to sender, route, item index, path, count and bytes.
void Engine::DecideItems(uint64_t id, const std::string &selection) {
    std::lock_guard lock(mutex_);
    for (auto &job : jobs_) if (job->view.id == id && job->view.requestOnly && job->view.awaitingApproval &&
        selection.size() == job->view.offerItems.size() && selection.find_first_not_of("01") == std::string::npos) {
        job->selection = selection;
        job->decision = selection.find('1') == std::string::npos ? -1 : 1;
        job->view.awaitingApproval = false; job->cv.notify_all();
    }
}
bool Engine::SendOffer(const Peer &peer, const std::string &batch, const std::vector<OfferItem> &items,
                       std::string &selection, std::wstring &error, const std::shared_ptr<Job> &control) {
    std::string manifest;
    for (auto &item : items) manifest += Hex(Utf8(item.name)) + "\t" + (item.folder ? "D" : "F") + "\t" +
        std::to_string(item.count) + "\t" + std::to_string(item.total) + "\n";
    const bool local = peer.browser && peer.owner.empty();
    SOCKET socket = Connect(local ? "127.0.0.1" : peer.gateway.empty() ? peer.ip : peer.gateway, local ? boundPort_ : peer.port);
    if (socket == INVALID_SOCKET) { error = L"无法连接目标设备"; return false; }
    TrackSocket(socket, control); SetTimeout(socket, 150000, 30000);
    Config cfg; { std::lock_guard lock(mutex_); cfg = config_; }
    const auto request = "POST /api/offer HTTP/1.1\r\nHost: " + (local ? "127.0.0.1" : peer.ip) + ":" + std::to_string(local ? boundPort_ : peer.port) +
        "\r\nX-Chuan-Id: " + Hex(Utf8(cfg.id)) + "\r\nX-Chuan-Port: " + std::to_string(boundPort_) +
        "\r\nX-Chuan-Sender: " + Hex(Utf8(cfg.name.empty() ? L"发送方" : cfg.name)) + "\r\nX-Chuan-Batch: " + batch +
        "\r\nX-Chuan-Display-Sender: " + Hex(Utf8(control ? control->senderName : L"")) +
        "\r\nX-Chuan-Recipient: " + (peer.browser ? Hex(Utf8(peer.id)) : "") +
        "\r\nConnection: close\r\nContent-Length: " + std::to_string(manifest.size()) + "\r\n\r\n";
    std::string first; std::map<std::string, std::string> headers; uint64_t length = 0;
    bool ok = SendAll(socket, request) && SendAll(socket, manifest) && ReadHeader(socket, first, headers);
    if (ok && first.starts_with("HTTP/1.1 200 ") && Number(Get(headers, "content-length"), length) && length == items.size()) {
        selection.assign(items.size(), '0');
        ok = ReadExact(socket, selection.data(), selection.size()) && selection.find_first_not_of("01") == std::string::npos;
    } else { ok = false; error = first.starts_with("HTTP/1.1 413 ") ? L"超过接收端设置的单批传输上限" : first.starts_with("HTTP/1.1 404 ") || first.starts_with("HTTP/1.1 405 ") ? L"接收端不支持整批审批，请更新接收端" : L"批次审批失败、连接中断或已超时"; }
    ReleaseSocket(socket); if (!ok && error.empty()) error = L"批次审批响应无效"; return ok;
}
void Engine::ReceiveOffer(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool browser) {
    const auto batchId = Get(headers, "x-chuan-batch");
    const auto owner = Get(headers, browser ? "x-chuan-client" : "x-chuan-id");
    auto sender = Wide(Unhex(Get(headers, Get(headers, "x-chuan-display-sender").empty() ? "x-chuan-sender" : "x-chuan-display-sender")));
    uint64_t length = 0;
    if (!WebKey(batchId) || owner.empty() || sender.empty() || sender.size() > 64 || sender.find_first_of(L"\r\n") != std::wstring::npos ||
        !Number(Get(headers, "content-length"), length) || !length || !Get(headers, "transfer-encoding").empty()) {
        Reply(socket, 400, "Bad Request"); return;
    }
    Config cfg; { std::lock_guard lock(mutex_); cfg = config_; }
    const auto target = Wide(Unhex(Get(headers, "x-chuan-target")));
    const auto recipient = Wide(Unhex(Get(headers, "x-chuan-recipient")));
    std::string recipientKey; Peer relay; bool forwarding = browser && !target.empty() && target != cfg.id;
    if (forwarding) {
        auto snapshot = GetSnapshot();
        auto found = std::find_if(snapshot.peers.begin(), snapshot.peers.end(), [&](auto &p) { return p.id == target; });
        if (found == snapshot.peers.end()) { Reply(socket, 409, "Conflict", Utf8(L"目标设备不在线")); return; }
        relay = *found;
    }
    if (!recipient.empty()) {
        std::lock_guard lock(mutex_);
        for (auto &[key, session] : browsers_) if (session.id == recipient && !session.tabs.empty()) {
            recipientKey = key;
            if (session.maxFiles && (!cfg.maxTransferFiles || session.maxFiles < cfg.maxTransferFiles)) cfg.maxTransferFiles = session.maxFiles;
            if (session.maxBytes && (!cfg.maxTransferBytes || session.maxBytes < cfg.maxTransferBytes)) cfg.maxTransferBytes = session.maxBytes;
        }
        if (browser || !cfg.shareSoftware || recipientKey.empty()) { Reply(socket, 409, "Conflict"); return; }
    }
    std::vector<OfferItem> items; uint64_t totalCount = 0, totalBytes = 0;
    std::set<std::wstring> roots;
    std::string line; bool valid = true, limit = false;
    std::array<char, 16384> buffer{};
    while (length && valid && !limit) {
        const auto want = static_cast<int>(std::min<uint64_t>(buffer.size(), length));
        const auto received = recv(socket, buffer.data(), want, 0);
        if (received <= 0) return;
        length -= received;
        for (int at = 0; at < received && valid && !limit; ++at) {
            const char value = buffer[at];
            if (value != '\n') { line += value; if (line.size() > 32768) valid = false; continue; }
            std::istringstream row(line); std::string name, type, count, bytes, extra;
            if (!std::getline(row, name, '\t') || !std::getline(row, type, '\t') || !std::getline(row, count, '\t') ||
                !std::getline(row, bytes) || bytes.find('\t') != std::string::npos) { valid = false; break; }
            OfferItem item; item.name = SanitizeRelative(Wide(Unhex(name))); item.folder = type == "D";
            if (item.name.empty() || item.name.find_first_of(L"\\/") != std::wstring::npos || (type != "D" && type != "F") ||
                !Number(count, item.count) || !item.count || (!item.folder && item.count != 1) || !Number(bytes, item.total) ||
                item.count > UINT64_MAX - totalCount || item.total > UINT64_MAX - totalBytes || !roots.insert(item.name).second) { valid = false; break; }
            totalCount += item.count; totalBytes += item.total;
            limit = (cfg.maxTransferFiles && totalCount > cfg.maxTransferFiles) || (cfg.maxTransferBytes && totalBytes > cfg.maxTransferBytes);
            items.push_back(std::move(item)); line.clear();
        }
    }
    if (limit) { Reply(socket, 413, "Content Too Large", Utf8(L"超过接收端设置的单批文件数量或总大小上限")); return; }
    if (!valid || !line.empty() || items.empty()) { Reply(socket, 400, "Bad Request"); return; }
    const auto key = (browser ? "web:" : "native:") + ip + ":" + owner + ":" + batchId;
    { std::lock_guard lock(mutex_);
      std::erase_if(batchApprovals_, [](const auto &entry) { const auto &batch = entry.second; bool complete = true; for (size_t i = 0; i < batch.items.size(); ++i) if (batch.selection[i] == '1' && batch.counts[i] < batch.items[i].count) complete = false; return complete && Now() - batch.touched > 24ull * 60 * 60 * 1000; });
      if (batchApprovals_.contains(key)) { Reply(socket, 409, "Conflict", Utf8(L"批次标识已使用，请重新发送")); return; }
      // Reserve this identifier before exposing the prompt, preventing parallel replay.
      batchApprovals_[key].touched = Now();
    }
    auto job = NewJob(items.size() == 1 ? items[0].name : std::to_wstring(totalCount) + L" 个文件", sender + L" (" + Wide(ip) + L")", totalBytes, true);
    { std::lock_guard lock(mutex_);
      job->view.requestOnly = true; job->view.offerItems = items; job->batchId = batchId; job->recipient = recipientKey;
      job->webClient = browser ? owner : ""; job->senderName = sender;
      job->view.state = State::Waiting;
      if (items.size() == 1 && items[0].folder) { job->view.folderName = items[0].name; job->view.folderCount = totalCount; job->view.folderTotal = totalBytes; }
      job->view.awaitingApproval = !forwarding && (!cfg.autoReceive || !recipientKey.empty());
    }
    TrackSocket(socket, job);
    std::string selection(items.size(), '1');
    if (forwarding) {
        std::wstring error;
        if (!SendOffer(relay, batchId, items, selection, error, job)) {
            SetState(job, State::Failed, error); { std::lock_guard lock(mutex_); job->hidden = true; }
            Reply(socket, 409, "Conflict", Utf8(error)); return;
        }
    } else if (!cfg.autoReceive || !recipientKey.empty()) {
        std::unique_lock lock(mutex_); const auto deadline = Now() + 120000;
        while (!job->decision && !job->cancel && running_ && Now() < deadline) {
            job->cv.wait_for(lock, std::chrono::milliseconds(250));
            lock.unlock(); fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable); timeval immediate{0,0}; char byte = 0;
            const bool disconnected = select(0, &readable, nullptr, nullptr, &immediate) > 0 && recv(socket, &byte, 1, MSG_PEEK) <= 0;
            lock.lock(); if (disconnected) job->cancel = true;
        }
        selection = job->selection.size() == items.size() && !job->cancel && running_ ? job->selection : std::string(items.size(), '0');
    }
    { std::lock_guard lock(mutex_);
      auto &approval = batchApprovals_.at(key); approval.items = std::move(items); approval.selection = selection;
      approval.target = Get(headers, "x-chuan-target"); approval.recipient = Get(headers, "x-chuan-recipient");
      approval.counts.resize(selection.size()); approval.bytes.resize(selection.size()); approval.touched = Now();
      job->view.awaitingApproval = false; job->view.state = selection.find('1') == std::string::npos ? State::Rejected : State::Completed;
      job->hidden = true; job->finishedAt = Now();
    }
    Reply(socket, 200, "OK", selection);
}
