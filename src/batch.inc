// Consent is tracked independently per item. The ledger binds every accepted
// payload to sender, route, original item index, path, count and bytes.
void Engine::ReceiveBatchFiles(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool status, bool browserRequest) {
    const auto owner = Get(headers, browserRequest?"x-chuan-client":"x-chuan-id"), batchId = Get(headers, "x-chuan-batch");
    uint64_t length = 0;
    if (!WebKey(batchId) || !Number(Get(headers, "content-length"), length) || !length || length > 16 * 1024 * 1024 || !Get(headers, "transfer-encoding").empty()) {
        Reply(socket, 400, "Bad Request"); return;
    }
    std::string body(static_cast<size_t>(length), '\0');
    if (!ReadExact(socket, body.data(), body.size())) return;
    struct Entry { uint64_t item = 0, size = 0, state = 0; std::string upload, folder; std::wstring name, error; bool archive=false; };
    std::vector<Entry> entries; std::istringstream input(body); std::string line;
    while (std::getline(input, line)) {
        std::vector<std::string> fields; std::istringstream row(line); std::string field;
        while (std::getline(row, field, '\t')) fields.push_back(field);
        Entry entry;
        if (status) {
            if (fields.size() != 3 || !WebKey(fields[0]) || !Number(fields[1], entry.state) || (entry.state != 4 && entry.state != 5)) { Reply(socket, 400, "Bad Request"); return; }
            entry.upload = fields[0]; entry.error = Wide(Unhex(fields[2]));
            if (entry.error.empty() || entry.error.size() > 1024 || entry.error.find_first_of(L"\r\n") != std::wstring::npos) { Reply(socket, 400, "Bad Request"); return; }
        } else {
            if ((fields.size() != 5 && fields.size()!=6) || !Number(fields[0], entry.item) || !WebKey(fields[1]) || !Number(fields[4], entry.size)) { Reply(socket, 400, "Bad Request"); return; }
            entry.upload = fields[1]; entry.folder = fields[2]; entry.name = SanitizeRelative(Wide(Unhex(fields[3])));
            entry.archive=fields.size()==6&&fields[5]=="A";
            if(fields.size()==6&&(!entry.archive||!entry.name.ends_with(L".zip"))){Reply(socket,400,"Bad Request");return;}
            if (entry.name.empty()) { Reply(socket, 400, "Bad Request"); return; }
        }
        entries.push_back(std::move(entry));
        if (entries.size() > 50000) { Reply(socket, 413, "Content Too Large"); return; }
    }
    if (entries.empty()) { Reply(socket, 400, "Bad Request"); return; }
    std::lock_guard lock(mutex_);
    const auto found = batchApprovals_.find(std::string(browserRequest?"web:":"native:") + ip + ":" + owner + ":" + batchId);
    if (found == batchApprovals_.end() || found->second.selection.size() != found->second.items.size() ||
        (!found->second.target.empty() && Wide(Unhex(found->second.target)) != config_.id)) { Reply(socket, 409, "Conflict"); return; }
    auto &batch = found->second;
    if (status) {
        std::map<std::string,std::shared_ptr<Job>> owned;
        for (const auto &job:jobs_) if(job->batchId==batchId&&(browserRequest?job->webClient:job->nativeOwner)==owner)owned[job->webUpload]=job;
        for (const auto &entry:entries) if(!owned.contains(entry.upload)){Reply(socket,404,"Not Found");return;}
        for (const auto &entry : entries) {
            const auto &job=owned.at(entry.upload);if(job->view.state==State::Completed||job->cancel)continue;
            job->view.state=static_cast<State>(entry.state);job->view.error=entry.error;job->view.speed=0;
            if(entry.state==4){job->cancel=true;batch.cancelledItems.insert(job->batchItem);}
            job->announced=false;job->finishedAt=Now();job->cv.notify_all();
            diagnostic_->Write("\"event\":\"batch_file_status\",\"id\":"+std::to_string(job->view.id)+",\"name\":"+Json(Utf8(job->view.name))+",\"error\":"+Json(Utf8(entry.error)));
        }
        Reply(socket, 200, "OK"); return;
    }
    std::vector<uint64_t> counts(batch.items.size()), sizes(batch.items.size());
    std::set<std::wstring> paths; std::set<std::string> uploads; std::map<uint64_t,std::string> folders;
    for (const auto &entry : entries) {
        if (entry.item >= batch.items.size() || batch.selection[entry.item] != '1' || batch.announcedItems.contains(entry.item)) { Reply(socket, 400, "Bad Request"); return; }
        const auto &item = batch.items[entry.item];
        if(entry.archive&&item.folder){Reply(socket,400,"Bad Request");return;}
        if (item.folder) { auto [folder, inserted] = folders.try_emplace(entry.item, entry.folder); if (!inserted && folder->second != entry.folder) { Reply(socket, 400, "Bad Request"); return; } }
        if (!paths.insert(entry.name).second || !uploads.insert(entry.upload).second || entry.size > item.total - sizes[entry.item] ||
            (item.folder ? !WebKey(entry.folder) || !entry.name.starts_with(item.name + L"\\") : !entry.folder.empty() || entry.name != item.name)) { Reply(socket, 400, "Bad Request"); return; }
        ++counts[entry.item]; sizes[entry.item] += entry.size;
    }
    for (size_t i = 0; i < batch.items.size(); ++i) if (counts[i] && (counts[i] != batch.items[i].count || sizes[i] != batch.items[i].total)) { Reply(socket, 400, "Bad Request"); return; }
    const auto browser = browsers_.find(owner);
    if (browserRequest && browser == browsers_.end()) { Reply(socket, 403, "Forbidden"); return; }
    const auto sender = browserRequest?(browser->second.name.empty() ? AutoNameLocked(Utf8(browser->second.id)) : browser->second.name):Wide(Unhex(Get(headers,"x-chuan-sender")));
    if(sender.empty()||sender.size()>64){Reply(socket,400,"Bad Request");return;}
    for (const auto &entry : entries) {
        const auto &item = batch.items[entry.item]; auto job = std::make_shared<Job>();
        job->view.id = nextJob_++; job->view.name = entry.name; job->view.peer = sender + L" (" + Wide(ip) + L")";
        job->view.total = entry.size; job->view.receiving = true; job->view.state = State::Queued; job->view.started = Now();
        if(browserRequest){job->webClient=owner;job->webTab=Get(headers,"x-chuan-tab");}else job->nativeOwner=owner;
        job->webUpload = entry.upload; job->batchId = batchId; job->batchItem = static_cast<size_t>(entry.item);
        if(entry.archive)job->view.activity=L"等待发送端打包 ZIP";
        job->senderName = sender; job->targetId = config_.id; job->announced = true;
        if (item.folder) { job->folderId = entry.folder; job->view.folderId = entry.folder; job->view.folderName = item.name; job->view.folderCount = item.count; job->view.folderTotal = item.total; }
        jobs_.push_back(std::move(job));
    }
    for (const auto &entry : entries) batch.announcedItems.insert(entry.item);
    batch.announced = true; batch.touched = Now(); Reply(socket, 200, "OK");
}
int Engine::BatchCommand(const Peer &peer,const std::string &path,const std::string &batch,const std::string &body,std::string &response,const std::shared_ptr<Job> &job){
    SOCKET socket=Connect(peer.gateway.empty()?peer.ip:peer.gateway,peer.port);if(socket==INVALID_SOCKET)return 0;
    TrackSocket(socket,job);SetTimeout(socket,5000,5000);Config cfg;{std::lock_guard lock(mutex_);cfg=config_;}
    const auto request="POST "+path+" HTTP/1.1\r\nHost: "+peer.ip+":"+std::to_string(peer.port)+"\r\nX-Chuan-Id: "+Hex(Utf8(cfg.id))+"\r\nX-Chuan-Server: "+Utf8(peer.owner.empty()?peer.id:peer.owner)+"\r\nX-Chuan-Platform: windows\r\nX-Chuan-Port: "+std::to_string(boundPort_)+"\r\nX-Chuan-Sender: "+Hex(Utf8(cfg.name))+"\r\nX-Chuan-Batch: "+batch+"\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n";
    std::string first;std::map<std::string,std::string> headers;uint64_t length=0;int status=0;
    if(SendAll(socket,request)&&SendAll(socket,body)&&ReadHeader(socket,first,headers)&&first.size()>=12){status=std::atoi(first.substr(9,3).c_str());if(Number(Get(headers,"content-length"),length)&&length<=131072){response.assign(static_cast<size_t>(length),'\0');if(length&&!ReadExact(socket,response.data(),response.size()))status=0;}}
    ReleaseSocket(socket);return status;
}
void Engine::ReceiveOfferStatus(SOCKET socket, const std::map<std::string, std::string> &headers, const std::string &ip, bool browser, bool cancel) {
    const auto owner = Get(headers, browser ? "x-chuan-client" : "x-chuan-id");
    const auto key = std::string(browser ? "web:" : "native:") + ip + ":" + owner + ":" + Get(headers, "x-chuan-batch");
    uint64_t length = 0; if (!Number(Get(headers,"content-length"),length) || length > 65536 || !Get(headers,"transfer-encoding").empty()) { Reply(socket,400,"Bad Request");return; }
    std::string mask(static_cast<size_t>(length),' '); if (length && !ReadExact(socket,mask.data(),mask.size()))return;
    std::vector<uint64_t> cancelled;
    std::string response;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &job){return job->asyncOfferKey == key;});
        if (found == jobs_.end()) { Reply(socket,404,"Not Found");return; }
        const auto &job = *found;
        if (cancel) {
            if (mask.size()!=job->view.offerItems.size() || mask.find_first_not_of("0-")!=std::string::npos) { Reply(socket,400,"Bad Request");return; }
            CompleteOfferLocked(job,mask);
            for (size_t i=0;i<mask.size();++i) if(mask[i]=='0') {
                batchApprovals_.at(key).cancelledItems.insert(i);
                for(const auto &item:jobs_) if(!item->view.requestOnly && item->batchId==job->batchId && item->batchItem==i &&
                    (browser?item->webClient:item->nativeOwner)==owner && item->view.state<State::Completed) cancelled.push_back(item->view.id);
            }
        }
        batchApprovals_.at(key).touched=Now();
        response=std::to_string(static_cast<int>(job->view.state))+"\t"+job->selection+"\t"+Hex(Utf8(job->view.error));
    }
    for(auto id:cancelled)Cancel(id);
    Reply(socket,200,"OK",response);
}
void Engine::CompleteOfferLocked(const std::shared_ptr<Job> &job, const std::string &selection) {
    if (job->view.state >= State::Completed) return;
    auto &approval = batchApprovals_.at(job->asyncOfferKey);
    if (approval.selection.size() != approval.items.size()) approval.selection.assign(approval.items.size(), '-');
    for (size_t i = 0; i < approval.items.size(); ++i)
        if (approval.selection[i] == '-' && (job->cancel || selection[i] != '-')) approval.selection[i] = job->cancel ? '0' : selection[i];
    approval.counts.resize(approval.items.size()); approval.bytes.resize(approval.items.size()); approval.touched = Now();
    job->selection = approval.selection; job->view.offerSelection = approval.selection;
    const bool pending = approval.selection.find('-') != std::string::npos;
    job->decision = pending ? 0 : approval.selection.find('1') == std::string::npos ? -1 : 1;
    if (!pending || job->cancel) {
        job->view.awaitingApproval = false;
        job->view.state = job->cancel ? State::Cancelled : job->decision < 0 ? State::Rejected : State::Completed;
        job->hidden = true; job->finishedAt = Now();
    }
    job->cv.notify_all();
}
void Engine::DecideItems(uint64_t id, const std::string &selection) {
    std::lock_guard lock(mutex_);
    for (auto &job : jobs_) if (job->view.id == id && job->view.requestOnly && job->view.awaitingApproval &&
        selection.size() == job->view.offerItems.size() && selection.find_first_not_of("01-") == std::string::npos) {
        if (!job->asyncOfferKey.empty()) CompleteOfferLocked(job, selection);
        else {
            if (job->selection.size() != selection.size()) job->selection.assign(selection.size(), '-');
            for (size_t i = 0; i < selection.size(); ++i) if (job->selection[i] == '-' && selection[i] != '-') job->selection[i] = selection[i];
            job->view.offerSelection = job->selection;
            job->decision = job->selection.find('-') != std::string::npos ? 0 : job->selection.find('1') == std::string::npos ? -1 : 1;
            job->view.awaitingApproval = !job->decision;
        }
        job->cv.notify_all();
    }
}
bool Engine::SendOffer(const Peer &peer, const std::string &batch, const std::vector<OfferItem> &items,
                       std::string &selection, std::wstring &error, const std::shared_ptr<Job> &control,
                       SOCKET waitingSocket, const std::function<void(const std::string &)> &progress,
                       const std::function<std::string()> &cancelled) {
    std::string manifest;
    for (auto &item : items) manifest += Hex(Utf8(item.name)) + "\t" + (item.folder ? "D" : "F") + "\t" +
        std::to_string(item.count) + "\t" + std::to_string(item.total) + "\n";
    const bool local = peer.browser && peer.owner.empty();
    SOCKET socket = Connect(local ? "127.0.0.1" : peer.gateway.empty() ? peer.ip : peer.gateway, local ? boundPort_ : peer.port);
    if (socket == INVALID_SOCKET) { error = L"无法连接目标设备"; return false; }
    TrackSocket(socket, control); SetTimeout(socket, 150000, 30000);
    Config cfg; { std::lock_guard lock(mutex_); cfg = config_; }
    const auto request = "POST /api/offer HTTP/1.1\r\nHost: " + (local ? "127.0.0.1" : peer.ip) + ":" + std::to_string(local ? boundPort_ : peer.port) +
        "\r\nX-Chuan-Id: " + Hex(Utf8(cfg.id)) + "\r\nX-Chuan-Platform: windows\r\nX-Chuan-Port: " + std::to_string(boundPort_) +
        "\r\nX-Chuan-Server: " + Utf8(local ? cfg.id : peer.owner.empty() ? peer.id : peer.owner) +
        "\r\nX-Chuan-Sender: " + Hex(Utf8(cfg.name.empty() ? L"发送方" : cfg.name)) + "\r\nX-Chuan-Batch: " + batch +
        "\r\nX-Chuan-Display-Sender: " + Hex(Utf8(control ? control->senderName : L"")) +
        "\r\nX-Chuan-Recipient: " + (peer.browser ? Hex(Utf8(peer.id)) : "") +
        "\r\nX-Chuan-Offer-Wait: 1\r\nX-Chuan-Offer-Async: 1" +
        "\r\nConnection: close\r\nContent-Length: " + std::to_string(manifest.size()) + "\r\n\r\n";
    std::string first; std::map<std::string, std::string> headers; uint64_t length = 0;
    bool ok = SendAll(socket, request) && SendAll(socket, manifest) && ReadHeader(socket, first, headers);
    // Interim responses keep a live approval pending without extending a dead connection.
    while (ok && first.starts_with("HTTP/1.1 102 ")) {
        if (control && control->cancel) { ok = false; error = L"发送已取消"; break; }
        if (waitingSocket != INVALID_SOCKET && !SendAll(waitingSocket, "HTTP/1.1 102 Processing\r\n\r\n")) {
            ok = false; error = L"发送端已断开连接"; break;
        }
        headers.clear(); ok = ReadHeader(socket, first, headers);
    }
    if (ok && first.starts_with("HTTP/1.1 202 ")) {
        ReleaseSocket(socket);
        Peer route = peer; if (local) { route.ip = "127.0.0.1"; route.gateway.clear(); route.port = boundPort_; route.owner = cfg.id; }
        selection.assign(items.size(), '-'); std::string last, sentCancel;
        uint64_t keepalive = Now(), failedAt = 0;
        while (running_) {
            const auto cancelMask = control && control->cancel ? std::string(items.size(), '0') : cancelled ? cancelled() : std::string();
            if (!cancelMask.empty() && cancelMask != sentCancel) {
                std::string response; if (BatchCommand(route, "/api/offer-cancel", batch, cancelMask, response, {}) == 200) sentCancel = cancelMask;
            }
            if (control && control->cancel) { error = L"发送已取消"; return false; }
            std::string response; const auto status = BatchCommand(route, "/api/offer-status", batch, "", response, control);
            if (status != 200) {
                if (!failedAt) failedAt = Now();
                if (status == 404 || Now() - failedAt > 15000) { error = L"无法获取接收确认，请检查连接后重试"; return false; }
            } else {
                failedAt = 0; const auto tab = response.find('\t');
                const auto end = response.find('\t', tab == std::string::npos ? response.size() : tab + 1);
                if (tab == std::string::npos || end == std::string::npos) { error = L"批次审批响应无效"; return false; }
                const auto state = std::atoi(response.c_str()); const auto mask = response.substr(tab + 1, end - tab - 1);
                if (mask.size() != items.size() || mask.find_first_not_of("01-") != std::string::npos) { error = L"批次审批响应无效"; return false; }
                if (state == 4 || state == 5) { error = Wide(Unhex(response.substr(end + 1))); if (error.empty()) error = L"接收申请已停止"; return false; }
                selection = mask;
                if (last != mask) { last = mask; if (progress) progress(mask); }
                if (mask.find('-') == std::string::npos) return true;
            }
            if (waitingSocket != INVALID_SOCKET && Now() - keepalive > 10000) {
                if (!SendAll(waitingSocket, "HTTP/1.1 102 Processing\r\n\r\n")) { error = L"发送端已断开连接"; return false; } keepalive = Now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        error = L"服务已停止"; return false;
    }
    if (ok && first.starts_with("HTTP/1.1 200 ") && Number(Get(headers, "content-length"), length) && length == items.size()) {
        selection.assign(items.size(), '0');
        ok = ReadExact(socket, selection.data(), selection.size()) && selection.find_first_not_of("01") == std::string::npos;
    } else { ok = false; error = first.starts_with("HTTP/1.1 413 ") ? L"超过接收端设置的单批传输上限" : first.starts_with("HTTP/1.1 404 ") || first.starts_with("HTTP/1.1 405 ") ? L"接收端不支持整批审批，请更新接收端" : L"批次审批失败、连接中断或已超时"; }
    ReleaseSocket(socket); if (ok && progress) progress(selection); if (!ok && error.empty()) error = L"批次审批响应无效"; return ok;
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
    const bool asynchronous = Get(headers, "x-chuan-offer-async") == "1";
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
    if (asynchronous && forwarding && relay.browser && relay.owner.empty()) {
        std::lock_guard lock(mutex_);
        for (const auto &[client, session] : browsers_) if (session.id == relay.id && !session.openTabs.empty()) {
            recipientKey = client;
            if (session.maxFiles && (!cfg.maxTransferFiles || session.maxFiles < cfg.maxTransferFiles)) cfg.maxTransferFiles = session.maxFiles;
            if (session.maxBytes && (!cfg.maxTransferBytes || session.maxBytes < cfg.maxTransferBytes)) cfg.maxTransferBytes = session.maxBytes;
        }
        if (recipientKey.empty()) { Reply(socket, 409, "Conflict", Utf8(L"目标浏览器已离线")); return; }
        forwarding = false;
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
      std::erase_if(batchApprovals_, [](const auto &entry) { const auto &batch = entry.second; bool complete = batch.selection.size() == batch.items.size() && batch.selection.find('-') == std::string::npos; for (size_t i = 0; i < batch.selection.size(); ++i) if (batch.selection[i] == '1' && batch.counts[i] < batch.items[i].count) complete = false; return complete && Now() - batch.touched > 24ull * 60 * 60 * 1000; });
      if (batchApprovals_.contains(key)) { Reply(socket, 409, "Conflict", Utf8(L"批次标识已使用，请重新发送")); return; }
      // Reserve this identifier before exposing the prompt, preventing parallel replay.
      batchApprovals_[key].touched = Now();
    }
    auto job = NewJob(items.size() == 1 ? items[0].name : std::to_wstring(totalCount) + L" 个文件", sender + L" (" + Wide(ip) + L")", totalBytes, true);
    { std::lock_guard lock(mutex_);
      job->view.requestOnly = true; job->view.offerItems = items; job->batchId = batchId; job->recipient = recipientKey;
      job->webClient = browser ? owner : "";job->nativeOwner = browser ? "" : owner;job->webTab=browser?Get(headers,"x-chuan-tab"):"";job->senderName = sender;
      job->selection.assign(items.size(), '-'); job->view.offerSelection = job->selection;
      job->view.state = State::Waiting;
      if (asynchronous) {
          job->asyncOfferKey = key;
          auto &approval = batchApprovals_.at(key); approval.items = items;
          approval.selection = job->selection; approval.counts.resize(items.size()); approval.bytes.resize(items.size());
          approval.target = Get(headers, "x-chuan-target"); approval.recipient = Get(headers, "x-chuan-recipient");
      }
      if (items.size() == 1 && items[0].folder) { job->view.folderName = items[0].name; job->view.folderCount = totalCount; job->view.folderTotal = totalBytes; }
      job->view.awaitingApproval = !forwarding && (!cfg.autoReceive || !recipientKey.empty());
    }
    if (asynchronous) {
        if (forwarding) {
            if (!Launch([this, relay, batchId, items, job] {
                std::string selection; std::wstring error;
                if (SendOffer(relay, batchId, items, selection, error, job, INVALID_SOCKET,
                    [this, job](const std::string &mask) { std::lock_guard lock(mutex_); CompleteOfferLocked(job, mask); },
                    [this, job] { std::lock_guard lock(mutex_); std::string mask(job->selection.size(), '-'); for (size_t i=0;i<mask.size();++i) if (job->selection[i]=='0') mask[i]='0'; return mask; })) {
                    std::lock_guard lock(mutex_); CompleteOfferLocked(job, selection);
                } else SetState(job, job->cancel ? State::Cancelled : State::Failed, error);
            })) SetState(job, State::Failed, L"服务繁忙，请稍后重试");
        } else if (cfg.autoReceive && recipientKey.empty()) {
            std::lock_guard lock(mutex_); CompleteOfferLocked(job, std::string(items.size(), '1'));
        }
        Reply(socket, 202, "Accepted", "{\"id\":" + Json(std::to_string(job->view.id)) + "}", "application/json; charset=utf-8");
        return;
    }
    TrackSocket(socket, job);
    std::string selection(items.size(), '1');
    if (forwarding) {
        std::wstring error;
        if (!SendOffer(relay, batchId, items, selection, error, job,
                       Get(headers, "x-chuan-offer-wait") == "1" ? socket : INVALID_SOCKET)) {
            SetState(job, State::Failed, error); { std::lock_guard lock(mutex_); job->hidden = true; }
            Reply(socket, 409, "Conflict", Utf8(error)); return;
        }
    } else if (!cfg.autoReceive || !recipientKey.empty()) {
        std::unique_lock lock(mutex_); auto keepalive = Now();
        while (!job->decision && !job->cancel && running_) {
            job->cv.wait_for(lock, std::chrono::milliseconds(250));
            lock.unlock(); fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable); timeval immediate{0,0}; char byte = 0;
            const bool disconnected = select(0, &readable, nullptr, nullptr, &immediate) > 0 && recv(socket, &byte, 1, MSG_PEEK) <= 0;
            lock.lock(); if (disconnected) job->cancel = true;
            if (!job->decision && !job->cancel && Get(headers, "x-chuan-offer-wait") == "1" && Now() - keepalive >= 10000) {
                lock.unlock(); const bool live = SendAll(socket, "HTTP/1.1 102 Processing\r\n\r\n"); lock.lock();
                keepalive = Now(); if (!live) job->cancel = true;
            }
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
