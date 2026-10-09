#define CHUAN_UI_TEST_BUILD
#include "../src/app.cpp"
#include <iostream>

static void PumpFor(unsigned milliseconds) {
    auto until = GetTickCount64() + milliseconds;
    MSG message{};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
}
static unsigned short TestPort(int type) {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET value = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(value, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    int size = sizeof(address);
    getsockname(value, reinterpret_cast<sockaddr *>(&address), &size);
    auto port = ntohs(address.sin_port);
    closesocket(value);
    WSACleanup();
    return port;
}
static bool RegisterBrowser(unsigned short port) {
    SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        return false;
    auto request = "GET /web/api/state HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                   "\r\nX-Chuan-Client: 44444444444444444444444444444444\r\n\r\n";
    send(value, request.data(), static_cast<int>(request.size()), 0);
    char buffer[8192];
    bool okay = recv(value, buffer, sizeof(buffer), 0) > 0;
    closesocket(value);
    return okay;
}
static bool PingWindow(unsigned short port, const std::wstring &id) {
    SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        closesocket(value); return false;
    }
    const auto request = "POST /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nX-Chuan-Target: " + Hex(Utf8(id)) + "\r\n\r\n";
    send(value, request.data(), static_cast<int>(request.size()), 0);
    char response[512]{};
    int count = recv(value, response, sizeof(response), 0);
    closesocket(value);
    return count > 0 && std::string(response, count).starts_with("HTTP/1.1 200 ");
}

int wmain(int argc, wchar_t **argv) {
    if (argc > 3)
        return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    auto data = argc == 1 ? ExePath().parent_path() / L"ui-native-preview" : fs::absolute(argv[1]);
    Config cfg = LoadConfig(data);
    cfg.name = L"UI idle check";
    cfg.receiveDir = data / L"received";
    cfg.port = TestPort(SOCK_STREAM);
    cfg.discoveryPort = TestPort(SOCK_DGRAM);
    Engine engine(cfg); // Rendering checks do not open any network sockets.
    App app(engine);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"ChuanUiRegression";
    RegisterClassW(&cls);
    HWND window =
        CreateWindowExW(0, cls.lpszClassName, L"Chuan UI regression", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        0, 0, 1040, 820, nullptr, nullptr, cls.hInstance, &app);
    if (!window || !app.factory || !app.write)
        return 3;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    PumpFor(1200);
    auto beforeMain = app.paintCount, beforeControls = app.controlPaintCount;
    if (!beforeMain || !beforeControls)
        return 4;
    if (IsWindowEnabled(app.drop) || app.dropEnabled || !app.peerId.empty())
        return 10;
    PumpFor(1600); // Four timer intervals must not trigger any idle redraw.
    int code = 0;
    if (app.paintCount != beforeMain || app.controlPaintCount != beforeControls)
        code = 5;
    std::cout << "Idle paint counts: main " << beforeMain << " -> " << app.paintCount << ", controls "
              << beforeControls << " -> " << app.controlPaintCount << '\n';
    std::wstring error;
    cfg.name = L"Changed device name";
    if (!engine.UpdateConfig(cfg, error))
        code = 6;
    app.Update();
    PumpFor(400);
    if (app.paintCount <= beforeMain)
        code = 7;
    auto changedMain = app.paintCount, changedControls = app.controlPaintCount;
    PumpFor(1200);
    if (app.paintCount != changedMain || app.controlPaintCount != changedControls)
        code = 8;
    wchar_t status[256]{};
    GetWindowTextW(app.status, status, 256);
    if (std::wstring(status).find(cfg.name) == std::wstring::npos && !app.snapshot.addresses.empty())
        code = 9;
    if (!engine.Start(error) || !RegisterBrowser(cfg.port))
        code = 11;
    app.Update();
    if (app.shownPeers.empty() || IsWindowEnabled(app.drop) || !app.peerId.empty())
        code = 12;
    SendMessageW(app.devices, LB_SETCURSEL, 0, 0);
    app.Command(Devices, LBN_SELCHANGE);
    if (!IsWindowEnabled(app.drop) || !app.dropEnabled)
        code = 13;
    cfg.shareSoftware = false;
    engine.UpdateConfig(cfg, error);
    app.Update();
    if (IsWindowEnabled(app.drop) || app.dropEnabled || !app.peerId.empty())
        code = 14;
    cfg.shareSoftware = true;
    engine.UpdateConfig(cfg, error);
    if (!PingWindow(cfg.port, engine.GetSnapshot().config.id)) code = 15;
    app.Update();
    if (!app.flash || !IsWindowVisible(app.flash) || GetParent(app.flash) != window) code = 16;
    auto beforeFlashRestore = app.paintCount;
    KillTimer(window, 2);
    for (unsigned pulse = 0; pulse < 3; ++pulse) {
        app.RenderPing(pulse * 600 + 300);
        BYTE alpha = 255; DWORD flags = 0;
        if (!GetLayeredWindowAttributes(app.flash, nullptr, &alpha, &flags) || alpha != 170 || !(flags & LWA_ALPHA)) code = 19;
        if (!(GetWindowLongPtrW(app.flash, GWL_EXSTYLE) & WS_EX_TRANSPARENT)) code = 20;
        app.RenderPing(pulse * 600 + 599);
        GetLayeredWindowAttributes(app.flash, nullptr, &alpha, &flags);
        if (alpha > 1) code = 21;
    }
    app.RenderPing(1800);
    PumpFor(50);
    if (IsWindowVisible(app.flash) || !IsWindowVisible(window)) code = 17;
    if (app.paintCount <= beforeFlashRestore) code = 18;
    RegisterBrowser(cfg.port);
    fs::path fixture=data/L"设计资料";
    fs::create_directories(fixture/L"图纸");
    { std::ofstream(fixture/L"说明.txt") << "folder overview"; std::ofstream(fixture/L"图纸"/L"平面图.txt") << "nested drawing"; }
    for (auto &peer : engine.GetSnapshot().peers) if (peer.browser) { engine.SendFiles(peer,{fixture}); break; }
    PumpFor(200); app.Update();
    if (app.shownJobs.size()!=1 || !app.shownJobs[0].folderRow) code=22;
    else {
        app.selectedTask=app.shownJobs[0].id; app.ToggleTask(VK_RIGHT);
        if (app.shownJobs.size()!=1 || SendMessageW(app.tasks,LB_GETCOUNT,0,0)!=1) code=23;
        app.Update();
        if (app.shownJobs.size()!=1) code=24;
        app.ToggleTask(VK_LEFT);
        if (app.shownJobs.size()!=1) code=25;
    }
    if (argc == 1 || (argc == 3 && (std::wstring(argv[2]) == L"--preview" || std::wstring(argv[2]) == L"--preview-mask"))) {
        std::cout << "PREVIEW port " << cfg.port << std::endl;
        app.ToggleTask(VK_RIGHT);
        if (argc == 3 && std::wstring(argv[2]) == L"--preview-mask") {
            ShowWindow(app.flash, SW_SHOWNOACTIVATE);
            app.RenderPing(300);
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        PumpFor(300000);
    }
    DestroyWindow(window);
    CoUninitialize();
    if (!code)
        std::cout << "PASS idle redraw, changed-content repaint and explicit destination gating\n";
    else
        std::cerr << "UI regression failed, code " << code << '\n';
    return code;
}
