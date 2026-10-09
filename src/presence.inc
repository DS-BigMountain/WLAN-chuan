// Included in core.cpp to share the bounded HTTP and socket helpers.
std::wstring Engine::AutoNameLocked(const std::string &key) {
    auto [it, inserted] = aliases_.try_emplace(key, 0);
    if (inserted) it->second = nextAlias_++;
    return L"A" + std::to_wstring(it->second);
}
void Engine::ExpireLocked() {
    const auto now = Now();
    for (auto &[key, session] : browsers_) {
        std::erase_if(session.tabs, [&](auto &tab) { return now - tab.second >= 6000; });
        if (session.tabs.empty())
            std::erase_if(peers_, [&](auto &p) { return p.id == session.id; });
    }
    std::erase_if(peers_, [&](auto &p) { return now - p.seen >= 6000; });
    for (auto &job : jobs_) {
        if (job->recipient.empty() || job->view.state != State::Waiting) continue;
        auto session = browsers_.find(job->recipient);
        if (session != browsers_.end() && !session->second.tabs.empty()) continue;
        job->cancel = true;
        job->view.state = State::Cancelled;
        job->view.error = L"浏览器设备已离线";
        job->finishedAt = now;
        job->cv.notify_all();
    }
    // A running native host itself keeps this LAN session alive. The ledger is memory-only.
}
void Engine::SetNote(const std::string &ip, const std::wstring &note) {
    if (note.size() > 48 || note.find_first_of(L"\r\n|") != std::wstring::npos) return;
    std::lock_guard lock(mutex_); notes_[ip] = note;
}
std::wstring Engine::GetNote(const std::string &ip) {
    std::lock_guard lock(mutex_);
    auto it = notes_.find(ip); return it == notes_.end() ? L"" : it->second;
}
bool Engine::RequestPeer(const Peer &peer, const std::string &path, const std::string &extra, std::string &body) {
    const auto address = peer.gateway.empty() ? peer.ip : peer.gateway;
    SOCKET s = Connect(address, peer.port);
    if (s == INVALID_SOCKET) return false;
    TrackSocket(s); SetTimeout(s, 4500, 2000);
    Config config;
    { std::lock_guard lock(mutex_); config = config_; }
    const bool mesh = path == "/api/mesh";
    const auto method = mesh || path == "/api/ping" || path == "/api/leave" ? "POST " : "GET ";
    std::string request = std::string(method) + path + " HTTP/1.1\r\nHost: " + address + ":" +
        std::to_string(peer.port) + "\r\nConnection: close\r\nContent-Length: " + std::to_string(mesh ? extra.size() : 0) + "\r\nX-Chuan-Id: " +
        Hex(Utf8(config.id)) + "\r\nX-Chuan-Sender: " + Hex(Utf8(config.name)) +
        "\r\nX-Chuan-Port: " + std::to_string(boundPort_) + "\r\n" + (mesh ? "\r\n" + extra : extra + "\r\n");
    std::string first; std::map<std::string, std::string> headers; uint64_t length = 0;
    bool ok = SendAll(s, request) && ReadHeader(s, first, headers) && first.starts_with("HTTP/1.1 200 ") &&
        Number(Get(headers, "content-length"), length) && length <= 1024 * 1024;
    if (ok) { body.resize(static_cast<size_t>(length)); ok = ReadExact(s, body.data(), body.size()); }
    ReleaseSocket(s); return ok;
}
std::string Engine::Mesh(const std::string &ledger) {
    std::lock_guard lock(mutex_); ExpireLocked();
    std::wstring leader = config_.id;
    for (auto &p : peers_) if (!p.browser && p.id < leader) leader = p.id;
    std::istringstream input(ledger); std::string key; uint64_t number;
    while (input >> key >> number) {
        key = Unhex(key);
        if (key.empty() || key.size() > 128 || !number || number > 9007199254740991ull) continue;
        if (!aliases_.contains(key)) {
            bool used = std::any_of(aliases_.begin(), aliases_.end(), [&](auto &entry) { return entry.second == number; });
            aliases_[key] = used ? nextAlias_++ : number;
        }
        nextAlias_ = std::max(nextAlias_, aliases_[key] + 1);
    }
    if (leader == config_.id) {
        std::set<uint64_t> allocated;
        for (auto &[id, value] : aliases_) {
            if (!allocated.insert(value).second) { value = nextAlias_++; allocated.insert(value); }
        }
    }
    if (config_.name.empty()) AutoNameLocked(Utf8(config_.id));
    std::ostringstream out;
    out << "L " << Hex(Utf8(leader)) << '\n';
    for (auto &[id, value] : aliases_) out << "N " << Hex(id) << ' ' << value << '\n';
    for (auto &[keyValue, session] : browsers_) {
        if (session.tabs.empty()) continue;
        const auto name = session.name.empty() ? AutoNameLocked(Utf8(session.id)) : session.name;
        out << "B " << Hex(Utf8(session.id)) << ' ' << Hex(Utf8(name)) << ' ' << session.ip << '\n';
    }
    return out.str();
}
void Engine::SyncMesh(const Peer &peer) {
    std::string ledger;
    { std::lock_guard lock(mutex_);
      for (auto &[id, value] : aliases_) ledger += Hex(id) + " " + std::to_string(value) + "\n"; }
    std::string body;
    if (!RequestPeer(peer, "/api/mesh", ledger, body)) return;
    std::lock_guard lock(mutex_);
    std::wstring leader = config_.id;
    for (auto &p : peers_) if (!p.browser && Now() - p.seen < 6000 && p.id < leader) leader = p.id;
    bool authority = false;
    std::set<std::wstring> browserIds;
    std::istringstream lines(body); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line); std::string type, id, name, ip; uint64_t number;
        fields >> type >> id;
        if (type == "L") authority = Wide(Unhex(id)) == leader && peer.id == leader;
        else if (type == "N" && fields >> number && number && number < 9007199254740991ull) {
            auto key = Unhex(id); if (key.empty() || key.size() > 128) continue;
            if (authority || !aliases_.contains(key)) aliases_[key] = number;
            nextAlias_ = std::max(nextAlias_, number + 1);
        } else if (type == "B" && fields >> name >> ip && IsLocal(ip)) {
            Peer browser{Wide(Unhex(id)), Wide(Unhex(name)), ip, peer.port, Now(), false, true};
            if (!browser.id.starts_with(L"web:") || browser.name.size() > 64) continue;
            browser.gateway = peer.ip; browser.owner = peer.id;
            browserIds.insert(browser.id);
            auto found = std::find_if(peers_.begin(), peers_.end(), [&](auto &p) { return p.id == browser.id; });
            if (found == peers_.end()) peers_.push_back(browser);
            else if (found->owner == peer.id) *found = browser;
        }
    }
    std::erase_if(peers_, [&](auto &p) { return p.owner == peer.id && !browserIds.contains(p.id); });
    for (auto &[key, session] : browsers_) if (session.name.empty())
        for (auto &p : peers_) if (p.id == session.id) p.name = AutoNameLocked(Utf8(session.id));
}
bool Engine::PingNow(const Peer &peer, std::wstring &error) {
    if (peer.browser && peer.owner.empty()) {
        std::string key; uint64_t sequence = 0;
        { std::lock_guard lock(mutex_);
          for (auto &[client, session] : browsers_) if (session.id == peer.id && !session.tabs.empty()) {
              key = client; sequence = ++session.ping; session.pingAt = Now(); break;
          } }
        if (key.empty()) { error = L"目标设备已离线"; return false; }
        auto deadline = Now() + 3500;
        while (running_ && Now() < deadline) {
            { std::lock_guard lock(mutex_); if (browsers_.at(key).ack >= sequence) return true; }
            Sleep(30);
        }
        error = L"页面未确认 Ping，可能已暂停或离线"; return false;
    }
    std::string body;
    bool ok = RequestPeer(peer, "/api/ping", "X-Chuan-Target: " + Hex(Utf8(peer.id)) + "\r\n", body);
    if (!ok) error = L"Ping 超时或目标设备不可达";
    return ok;
}
void Engine::Ping(const Peer &peer) {
    { std::lock_guard lock(mutex_); auto &at = pingTimes_["desktop"];
      if (Now() - at < 800) return; at = Now(); }
    if (!Launch([this, peer] { std::wstring error; Notice(PingNow(peer, error) ? L"Ping 已送达" : error); }))
        Notice(L"当前任务较多，请稍后重试");
}
void Engine::CleanRelay() {
    std::vector<uint64_t> expired;
    {
        std::lock_guard lock(mutex_);
        for (auto &j : jobs_) if (j->staging && (!running_ ||
            (j->view.state >= State::Completed && j->finishedAt && Now() - j->finishedAt > 600000))) {
            if (!running_) j->view.state = State::Cancelled;
            expired.push_back(j->view.id);
        }
    }
    for (auto id : expired) { std::wstring error; DeleteTransfer(id, true, error); }
}
