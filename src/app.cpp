#include "core.h"
#include "ui.h"
#include "transfer_view.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <uxtheme.h>
#include <windowsx.h>

using namespace chuan;
namespace {
constexpr int Devices = 100, Drop = 101, Settings = 102, Refresh = 103, ConnectPeer = 104, CopyUrl = 105,
              OpenUrl = 106, Tasks = 107, Cancel = 108, Clear = 109, Accept = 110, Reject = 111,
              OpenFolder = 112, WebService = 113, Delete = 114, PingPeer = 115, NotePeer = 116, Partial = 117;
constexpr COLORREF Ink = RGB(45, 45, 45), Muted = RGB(115, 115, 115), Line = RGB(228, 228, 228);
struct App {
    Engine &engine;
    HWND window = nullptr, devices = nullptr, tasks = nullptr, drop = nullptr, request = nullptr,
         status = nullptr, accept = nullptr, reject = nullptr, cancel = nullptr, partial = nullptr;
    HWND settingsButton = nullptr, refreshButton = nullptr, connectButton = nullptr, clearButton = nullptr,
         folderButton = nullptr, webButton = nullptr, deleteButton = nullptr;
    HWND pingButton = nullptr, noteButton = nullptr, flash = nullptr;
    uint64_t displayedPing = 0;
    uint64_t pingStarted = 0;
    bool dropEnabled = false;
    HFONT font = nullptr, smallFont = nullptr, titleFont = nullptr;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), gray = CreateSolidBrush(RGB(243, 243, 243)),
           surface = CreateSolidBrush(RGB(249, 249, 249)), pingBrush = CreateSolidBrush(RGB(210, 210, 210));
    ID2D1Factory *factory = nullptr;
    ID2D1HwndRenderTarget *target = nullptr;
    ID2D1DCRenderTarget *controlTarget = nullptr;
    IDWriteFactory *write = nullptr;
    IDWriteTextFormat *normal = nullptr, *captionFormat = nullptr, *heading = nullptr, *section = nullptr;
    Snapshot snapshot;
    std::wstring peerId, taskKey;
    uint64_t selectedTask = 0, pending = 0;
    bool pendingPartial = false;
    float scale = 1;
    int width = 980, height = 700, jobTop = 345;
    std::vector<Peer> shownPeers;
    std::vector<TransferRow> shownJobs;
    std::set<std::string> expandedFolders;
    std::wstring localNotice, displayedPeer;
    uint64_t paintCount = 0, controlPaintCount = 0;
    explicit App(Engine &e) : engine(e) {}
    ~App() {
        DiscardTarget();
        if (controlTarget)
            controlTarget->Release();
        if (section)
            section->Release();
        if (normal)
            normal->Release();
        if (captionFormat)
            captionFormat->Release();
        if (heading)
            heading->Release();
        if (write)
            write->Release();
        if (factory)
            factory->Release();
        DeleteObject(font);
        DeleteObject(smallFont);
        DeleteObject(titleFont);
        DeleteObject(white);
        DeleteObject(gray);
        DeleteObject(surface);
        DeleteObject(pingBrush);
    }
    int Px(float dip) const {
        return static_cast<int>(dip * scale + .5f);
    }
    HFONT MakeFont(int size, int weight = FW_NORMAL) {
        return CreateFontW(-Px(static_cast<float>(size)), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
    }
    void Fonts() {
        if (font)
            DeleteObject(font);
        if (smallFont)
            DeleteObject(smallFont);
        if (titleFont)
            DeleteObject(titleFont);
        font = MakeFont(14);
        smallFont = MakeFont(12);
        titleFont = MakeFont(20, FW_SEMIBOLD);
        EnumChildWindows(
            window,
            [](HWND h, LPARAM p) {
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(reinterpret_cast<App *>(p)->font), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
    }
    static LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
        if (msg == WM_MOUSEMOVE && !GetPropW(h, L"ChuanHover")) {
            SetPropW(h, L"ChuanHover", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (msg == WM_MOUSELEAVE) {
            RemovePropW(h, L"ChuanHover");
            InvalidateRect(h, nullptr, FALSE);
        }
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_NCDESTROY) {
            RemovePropW(h, L"ChuanHover");
            RemoveWindowSubclass(h, ButtonProc, 1);
        }
        return DefSubclassProc(h, msg, w, l);
    }
    static void SetTextIfChanged(HWND h, const std::wstring &text) {
        int length = GetWindowTextLengthW(h);
        std::wstring old(static_cast<size_t>(length) + 1, 0);
        GetWindowTextW(h, old.data(), length + 1);
        old.resize(length);
        if (old != text)
            SetWindowTextW(h, text.c_str());
    }
    static void EnableIfChanged(HWND h, bool enabled) {
        if ((IsWindowEnabled(h) != FALSE) != enabled)
            EnableWindow(h, enabled);
    }
    static LRESULT CALLBACK TaskProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto *app = reinterpret_cast<App *>(data);
        if (msg == WM_KEYDOWN && (w == VK_RETURN || w == VK_SPACE || w == VK_LEFT || w == VK_RIGHT || w == VK_DELETE)) {
            if (w == VK_RETURN || w == VK_SPACE || w == VK_LEFT || w == VK_RIGHT) {
                if (!app->ToggleTask(w) && w == VK_RETURN) app->OpenTaskFile();
            }
            else
                app->DeleteTask();
            return 0;
        }
        auto result = DefSubclassProc(h, msg, w, l);
        if (msg == WM_LBUTTONUP) {
            auto row = SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
            if (!HIWORD(row) && LOWORD(row) < app->shownJobs.size()) {
                app->selectedTask = app->shownJobs[LOWORD(row)].id;
                RECT bounds{}; GetClientRect(h, &bounds);
                int x = GET_X_LPARAM(l);
                if (x >= bounds.right - app->Px(58)) {
                    auto &job = app->shownJobs[LOWORD(row)];
                    if (job.state < State::Completed) app->CancelTask();
                    else app->DeleteTask();
                } else if (x >= bounds.right - app->Px(154)) app->OpenTaskFile();
                else app->ToggleTask();
            }
        }
        return result;
    }
    HWND Button(const wchar_t *text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(h, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }
    HWND Label(const wchar_t *text) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    void Initialize() {
        scale = GetDpiForWindow(window) / 96.0f;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(&write));
        if (write) {
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN",
                                    &normal);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN",
                                    &captionFormat);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"zh-CN",
                                    &heading);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"zh-CN",
                                    &section);
        }
        devices = CreateWindowExW(0, L"LISTBOX", L"附近设备",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                      LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Devices)),
                                  GetModuleHandleW(nullptr), nullptr);
        tasks = CreateWindowExW(0, L"LISTBOX", L"传输状态",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Tasks)),
                                GetModuleHandleW(nullptr), nullptr);
        drop = Button(L"将文件拖入此区域发送", Drop);
        settingsButton = Button(L"设置", Settings);
        refreshButton = Button(L"刷新", Refresh);
        connectButton = Button(L"手动连接", ConnectPeer);
        webButton = Button(L"Web服务", WebService);
        clearButton = Button(L"清除已结束", Clear);
        cancel = Button(L"取消传输", Cancel);
        deleteButton = Button(L"删除", Delete);
        pingButton = Button(L"Ping", PingPeer);
        noteButton = Button(L"IP 备注", NotePeer);
        SetWindowSubclass(tasks, TaskProc, 2, reinterpret_cast<DWORD_PTR>(this));
        accept = Button(L"接收", Accept);
        reject = Button(L"拒绝", Reject);
        partial = Button(L"接受部分", Partial);
        folderButton = Button(L"接收目录", OpenFolder);
        request = Label(L"");
        status = Label(L"");
        SendMessageW(devices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未发现在线设备"));
        SendMessageW(tasks, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"暂无传输任务"));
        Fonts();
        DragAcceptFiles(window, FALSE);
        SetTimer(window, 1, 350, nullptr);
        Update();
    }
    void Move(HWND h, int x, int y, int w, int v) {
        MoveWindow(h, Px(static_cast<float>(x)), Px(static_cast<float>(y)),
                   Px(static_cast<float>(std::max(0, w))), Px(static_cast<float>(std::max(0, v))), TRUE);
    }
    void Layout() {
        RECT r{};
        GetClientRect(window, &r);
        width = static_cast<int>((r.right - r.left) / scale);
        height = static_cast<int>((r.bottom - r.top) / scale);
        int left = 280, right = width - 32;
        Move(devices, 16, 137, 216, std::max(60, height - 397));
        SendMessageW(devices, LB_SETITEMHEIGHT, 0, Px(shownPeers.empty() ? 116.f : 84.f));
        Move(refreshButton, 194, 94, 34, 34);
        Move(pingButton, 16, height - 240, 98, 36);
        Move(noteButton, 122, height - 240, 102, 36);
        Move(connectButton, 16, height - 190, 208, 40);
        Move(folderButton, 16, height - 143, 208, 40);
        Move(settingsButton, right - 88, 29, 88, 38);
        Move(drop, left, 112, right - left, 180);
        jobTop = pending ? 434 : 352;
        Move(request, left + 12, 304, right - left - 24, 34);
        Move(reject, right - 288, 345, 84, 34);
        Move(partial, right - 196, 345, 92, 34);
        Move(accept, right - 96, 345, 96, 34);
        ShowWindow(request, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(accept, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(reject, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(partial, pending && pendingPartial ? SW_SHOW : SW_HIDE);
        ShowWindow(deleteButton, SW_HIDE);
        Move(clearButton, right - 120, jobTop - 43, 120, 32);
        ShowWindow(cancel, SW_HIDE);
        Move(tasks, left, jobTop, right - left, std::max(68, height - jobTop - 106));
        SendMessageW(tasks, LB_SETITEMHEIGHT, 0, Px(shownJobs.empty() ? 132.f : 60.f));
        Move(webButton, right - 116, height - 84, 116, 36);
        Move(status, 22, height - 29, width - 44, 20);
        SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
        if (target)
            target->Resize(D2D1::SizeU(static_cast<UINT32>(r.right), static_cast<UINT32>(r.bottom)));
        InvalidateRect(window, nullptr, FALSE);
    }
    void Update() {
        auto oldName = snapshot.config.name;
        snapshot = engine.GetSnapshot();
        if (snapshot.ping != displayedPing) {
            displayedPing = snapshot.ping;
            if (!flash) flash = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            RECT bounds{}; GetClientRect(window, &bounds);
            SetLayeredWindowAttributes(flash, 0, 0, LWA_ALPHA);
            SetWindowPos(flash, HWND_TOP, 0, 0, bounds.right, bounds.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            pingStarted = GetTickCount64();
            SetTimer(window, 2, 16, nullptr);
        }
        std::sort(snapshot.peers.begin(), snapshot.peers.end(),
                  [](auto &a, auto &b) { return a.name < b.name; });
        bool changed = shownPeers.size() != snapshot.peers.size();
        if (!changed)
            for (size_t i = 0; i < shownPeers.size(); ++i)
                if (shownPeers[i].id != snapshot.peers[i].id ||
                    shownPeers[i].name != snapshot.peers[i].name ||
                    shownPeers[i].ip != snapshot.peers[i].ip || shownPeers[i].port != snapshot.peers[i].port)
                    changed = true;
        if (changed) {
            shownPeers = snapshot.peers;
            SendMessageW(devices, WM_SETREDRAW, FALSE, 0);
            SendMessageW(devices, LB_RESETCONTENT, 0, 0);
            for (auto &p : shownPeers) {
                std::wstring label = p.name + L" · " + Wide(p.ip) + L":" + std::to_wstring(p.port);
                SendMessageW(devices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            if (shownPeers.empty())
                SendMessageW(devices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未发现在线设备"));
            SendMessageW(devices, WM_SETREDRAW, TRUE, 0);
        }
        int selected = -1;
        for (size_t i = 0; i < shownPeers.size(); ++i)
            if (shownPeers[i].id == peerId)
                selected = static_cast<int>(i);
        if (selected < 0)
            peerId.clear();
        bool canSend = selected >= 0;
        EnableIfChanged(drop, canSend);
        EnableIfChanged(pingButton, canSend);
        EnableIfChanged(noteButton, canSend);
        if (dropEnabled != canSend) {
            dropEnabled = canSend;
            DragAcceptFiles(window, canSend);
        }
        if (SendMessageW(devices, LB_GETCURSEL, 0, 0) != selected)
            SendMessageW(devices, LB_SETCURSEL, selected, 0);
        auto oldJobs = shownJobs;
        shownJobs = TransferRows(snapshot.transfers, expandedFolders);
        bool content = oldJobs.size() != shownJobs.size();
        if (!content)
            for (size_t i = 0; i < oldJobs.size(); ++i)
                if (oldJobs[i].id != shownJobs[i].id || oldJobs[i].state != shownJobs[i].state || oldJobs[i].completedFiles != shownJobs[i].completedFiles)
                    content = true;
        int top = static_cast<int>(SendMessageW(tasks, LB_GETTOPINDEX, 0, 0));
        int sel = -1;
        if (content) {
            SendMessageW(tasks, WM_SETREDRAW, FALSE, 0);
            SendMessageW(tasks, LB_RESETCONTENT, 0, 0);
            for (auto &j : shownJobs) {
                std::wstring label = (j.folderRow ? L"文件夹 " : L"") + j.name + L" · " + StateText(j.state, j.receiving) + L" · " + j.peer;
                SendMessageW(tasks, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            if (shownJobs.empty())
                SendMessageW(tasks, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"暂无传输任务"));
            SendMessageW(tasks, WM_SETREDRAW, TRUE, 0);
            SendMessageW(tasks, LB_SETTOPINDEX, top, 0);
        }
        for (size_t i = 0; i < shownJobs.size(); ++i)
            if (shownJobs[i].id == selectedTask)
                sel = static_cast<int>(i);
        if (sel < 0 && !shownJobs.empty()) {
            sel = 0;
            selectedTask = shownJobs[0].id;
        }
        if (SendMessageW(tasks, LB_GETCURSEL, 0, 0) != sel)
            SendMessageW(tasks, LB_SETCURSEL, sel, 0);
        uint64_t oldPending = pending;
        pending = 0;
        for (auto &j : snapshot.transfers)
            if (j.receiving && j.state == State::Waiting && j.awaitingApproval) {
                pending = j.id; pendingPartial = j.offerItems.size() > 1;
                std::wstring label = j.folderName.empty() ? L"接收确认：" + j.peer + L" · " + j.name + L" · " + FormatBytes(j.total) :
                    L"接收文件夹：" + j.folderName + L" · " + std::to_wstring(j.folderCount) + L" 个文件 · " + FormatBytes(j.folderTotal);
                SetTextIfChanged(request, label);
                SetTextIfChanged(accept, j.folderName.empty() ? L"全部接受" : L"接受文件夹");
                SetTextIfChanged(reject, j.folderName.empty() ? L"拒绝" : L"拒绝整批");
                break;
            }
        if (oldPending != pending)
            Layout();
        bool active =
            sel >= 0 && (shownJobs[sel].state == State::Queued || shownJobs[sel].state == State::Waiting ||
                         shownJobs[sel].state == State::Running);
        EnableIfChanged(cancel, active);
        EnableIfChanged(deleteButton, sel >= 0 && !active);
        EnableIfChanged(clearButton, std::any_of(shownJobs.begin(), shownJobs.end(), [](const auto &j) {
                            return j.state == State::Completed || j.state == State::Failed ||
                                   j.state == State::Cancelled || j.state == State::Rejected;
                        }));
        std::wstring state = snapshot.addresses.empty()
                                 ? L"未检测到局域网地址"
                                 : L"本机：" + snapshot.config.name + L" · " + Wide(snapshot.addresses[0]);
        state += snapshot.config.autoReceive ? L"   自动接收：开" : L"   自动接收：关";
        if (!localNotice.empty())
            state += L"   " + localNotice;
        else if (!snapshot.notice.empty())
            state += L"   " + snapshot.notice;
        SetTextIfChanged(status, state);
        std::wstring destination = L"请先选择在线设备";
        for (auto &p : shownPeers)
            if (p.id == peerId)
                destination = L"目标设备：" + p.name;
        if (destination != displayedPeer) {
            displayedPeer = destination;
            InvalidateRect(drop, nullptr, FALSE);
        }
        if (changed) {
            SendMessageW(devices, LB_SETITEMHEIGHT, 0, Px(shownPeers.empty() ? 116.f : 84.f));
            InvalidateRect(devices, nullptr, FALSE);
        }
        if (content) {
            SendMessageW(tasks, LB_SETITEMHEIGHT, 0, Px(shownJobs.empty() ? 132.f : 60.f));
            InvalidateRect(tasks, nullptr, FALSE);
        } else {
            for (size_t i = 0; i < shownJobs.size(); ++i) {
                auto &a = oldJobs[i];
                auto &b = shownJobs[i];
                if (a.done != b.done || a.total != b.total || a.speed != b.speed || a.error != b.error ||
                    a.name != b.name || a.peer != b.peer) {
                    RECT row{};
                    SendMessageW(tasks, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&row));
                    InvalidateRect(tasks, &row, FALSE);
                }
            }
        }
        if (changed || content || oldName != snapshot.config.name)
            InvalidateRect(window, nullptr, FALSE);
    }
    void DiscardTarget() {
        if (target) {
            target->Release();
            target = nullptr;
        }
    }
    void RenderPing(uint64_t elapsed) {
        if (elapsed >= 1800) {
            ShowWindow(flash, SW_HIDE);
            KillTimer(window, 2);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return;
        }
        const double phase = static_cast<double>(elapsed % 600) / 600.0;
        const BYTE alpha = static_cast<BYTE>(170.0 * (1.0 - std::cos(phase * 6.283185307179586)) / 2.0);
        SetLayeredWindowAttributes(flash, 0, alpha, LWA_ALPHA);
    }
    void Paint() {
        ++paintCount;
        PAINTSTRUCT ps{};
        BeginPaint(window, &ps);
        if (!factory) {
            FillRect(ps.hdc, &ps.rcPaint, white);
            EndPaint(window, &ps);
            return;
        }
        if (!target) {
            RECT r{};
            GetClientRect(window, &r);
            factory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(r.right, r.bottom)), &target);
            if (target)
                target->SetDpi(96 * scale, 96 * scale);
        }
        if (target) {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(.975f, .975f, .975f));
            ui::Canvas c(target);
            if (c.brush) {
                c.Box(0, 0, 248, static_cast<float>(height - 38), 0, .953f);
                c.Line(248, 0, 248, static_cast<float>(height - 38), .90f, 1);
                c.Line(0, static_cast<float>(height - 38), static_cast<float>(width),
                       static_cast<float>(height - 38), .90f, 1);
                c.Box(24, 28, 36, 36, 10, .19f);
                c.Icon(0, 30, 34, 1);
                c.Text(L"传", 74, 24, 110, 34, heading, .18f);
                c.Text(L"局域网文件传输", 74, 59, 146, 21, captionFormat, .45f);
                c.Text(L"附近设备", 24, 96, 108, 31, section, .21f);
                c.Line(24, static_cast<float>(height - 204), 224, static_cast<float>(height - 204), .88f, 1);
                c.Icon(1, 28, static_cast<float>(height - 81), .42f);
                c.Text(L"本机", 64, static_cast<float>(height - 88), 150, 20, captionFormat, .5f);
                c.Text(snapshot.config.name, 64, static_cast<float>(height - 68), 162, 21, captionFormat,
                       .23f);
                c.Text(L"文件传输", 280, 27, 230, 40, heading, .17f);
                c.Text(L"设备互联 · 文件直达", 280, 71, 310, 24, captionFormat, .47f);
                c.Line(280, 97, static_cast<float>(width - 32), 97, .9f, 1);
                c.Text(L"传输状态", 280, static_cast<float>(jobTop - 42), 110, 32, section, .21f);
                c.Text(std::to_wstring(std::count_if(shownJobs.begin(), shownJobs.end(), [](auto &j) { return !j.childRow; })), 391, static_cast<float>(jobTop - 42), 38, 32,
                       captionFormat, .5f);
            }
            if (target->EndDraw() == D2DERR_RECREATE_TARGET)
                DiscardTarget();
        }
        EndPaint(window, &ps);
    }
    void DrawItem(const DRAWITEMSTRUCT &d) {
        if (!factory || !write)
            return;
        ++controlPaintCount;
        bool side =
            d.CtlID == Devices || d.CtlID == Refresh || d.CtlID == ConnectPeer || d.CtlID == OpenFolder || d.CtlID == PingPeer || d.CtlID == NotePeer;
        bool modal = GetParent(d.hwndItem) != window;
        float background = modal || d.CtlID == CopyUrl || d.CtlID == OpenUrl ? 1.f : side ? .953f : .975f;
        Buffered(d.hDC, d.rcItem, background,
                 [&](ui::Canvas &c, float w, float h) { DrawControl(c, d, w, h, background); });
    }
    void Buffered(HDC destination, RECT rectangle, float background,
                  const std::function<void(ui::Canvas &, float, float)> &draw) {
        if (!factory)
            return;
        int pixelW = rectangle.right - rectangle.left, pixelH = rectangle.bottom - rectangle.top;
        if (pixelW <= 0 || pixelH <= 0)
            return;
        HDC memory = CreateCompatibleDC(destination);
        HBITMAP bitmap = CreateCompatibleBitmap(destination, pixelW, pixelH);
        if (!memory || !bitmap) {
            DeleteObject(bitmap);
            DeleteDC(memory);
            return;
        }
        auto previous = SelectObject(memory, bitmap);
        RECT bounds{0, 0, pixelW, pixelH};
        if (!controlTarget) {
            auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            factory->CreateDCRenderTarget(&properties, &controlTarget);
        }
        if (controlTarget && SUCCEEDED(controlTarget->BindDC(memory, &bounds))) {
            controlTarget->SetDpi(96 * scale, 96 * scale);
            controlTarget->SetTransform(D2D1::Matrix3x2F::Identity());
            controlTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            controlTarget->BeginDraw();
            float w = pixelW / scale, h = pixelH / scale;
            controlTarget->Clear(D2D1::ColorF(background, background, background));
            ui::Canvas c(controlTarget);
            if (c.brush)
                draw(c, w, h);
            HRESULT result = controlTarget->EndDraw();
            if (SUCCEEDED(result))
                BitBlt(destination, rectangle.left, rectangle.top, pixelW, pixelH, memory, 0, 0, SRCCOPY);
            if (result == D2DERR_RECREATE_TARGET) {
                controlTarget->Release();
                controlTarget = nullptr;
            }
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
    }
    void DrawControl(ui::Canvas &c, const DRAWITEMSTRUCT &d, float w, float h, float background) {
        bool selected = (d.itemState & ODS_SELECTED) != 0;
        bool disabled = (d.itemState & ODS_DISABLED) != 0;
        bool hover = GetPropW(d.hwndItem, L"ChuanHover") != nullptr;
        bool keyboardFocus = (d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT);
        if (d.CtlID == Devices) {
            if (shownPeers.empty()) {
                c.Icon(1, 12, 14, .53f);
                c.Text(L"未发现在线设备", 12, 47, w - 24, 25, normal, .4f);
                c.Text(L"可刷新或手动连接", 12, 74, w - 24, 20, captionFormat, .52f);
            } else if (d.itemID < shownPeers.size()) {
                auto &p = shownPeers[d.itemID];
                c.Box(1, 3, w - 2, h - 9, 12, selected ? 1 : background,
                      keyboardFocus ? .45f
                      : selected    ? .82f
                                    : -1);
                c.Box(13, 15, 34, 34, 8, .94f);
                c.Icon(p.browser ? 12 : 1, 18, 19, .34f);
                c.Text(p.name, 59, 10, w - 72, 24, normal, .18f);
                auto note = engine.GetNote(p.ip);
                c.Text(Wide(p.ip) + (note.empty() ? L"" : L" · " + note), 59, 33, w - 72, 21, captionFormat, .48f);
                c.Circle(63, 65, 2.5f, .4f, true);
                c.Text(p.browser ? L"在线 · 浏览器" : L"在线 · Windows", 72, 54, w - 85, 23, captionFormat,
                       .48f);
            }
            return;
        }
        if (d.CtlID == Tasks) {
            if (shownJobs.empty()) {
                c.Box(1, 1, w - 2, h - 2, 12, 1, .90f);
                c.Icon(2, w / 2 - 12, 24, .62f);
                c.Text(L"暂无传输任务", 12, 59, w - 24, 28, normal, .46f, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (d.itemID < shownJobs.size()) {
                auto &j = shownJobs[d.itemID];
                const float rowLeft = j.childRow ? 24.f : 1.f;
                c.Box(rowLeft, 1, w - rowLeft - 1, h - 4, 8, selected ? .97f : 1.f, keyboardFocus ? .5f : .9f);
                if (j.childRow) c.Line(18, 0, 18, h, .88f, 1);
                const float actionX = w - 154;
                const float indent = j.childRow ? 42.f : 0.f;

                c.Icon(j.folderRow ? 3 : 2, 12 + indent, 15, .49f, .9f);
                const float textX = 44 + indent;
                auto title = j.name + (j.folderRow ? L"  ·  " + std::to_wstring(j.completedFiles) + L"/" + std::to_wstring(j.folderCount) + L" 个文件" : L"");
                c.Text(title, textX, 5, actionX - textX - 12, 23, normal, .19f);
                std::wstring detail = (j.receiving ? L"接收 · " : L"发送 · ") + j.peer + L" · " + FormatBytes(j.total) + L" · " + StateText(j.state, j.receiving);
                if (j.folderRow) detail = (j.receiving ? L"接收 · " : L"发送 · ") + j.peer + L" · " + (j.state == State::Waiting ? L"等待接收或后续文件" : StateText(j.state, j.receiving)) + L" · " + FormatBytes(j.done) + L"/" + FormatBytes(j.total);
                if (j.state < State::Completed) {
                    detail += L" · " + std::to_wstring(j.total ? static_cast<unsigned>(100.0 * j.done / j.total) : j.completedFiles == j.folderCount && j.folderRow ? 100 : 0) + L"%";
                    if (!j.folderRow && !j.childRow) detail += L" · " + FormatBytes(static_cast<uint64_t>(j.state == State::Running ? j.speed : 0)) + L"/s";
                }
                if (!j.error.empty()) detail = j.error;
                const bool grouped = j.folderRow || j.childRow;
                c.Text(detail, textX, 28, actionX - textX - (grouped ? 94 : 12), 22, captionFormat, .48f);
                if (grouped) c.Text(FormatBytes(static_cast<uint64_t>(j.state == State::Running ? j.speed : 0)) + L"/s", actionX - 90, 28, 78, 22, captionFormat, .48f, DWRITE_TEXT_ALIGNMENT_TRAILING);
                if (j.state < State::Completed) {
                    const float fraction = j.total ? static_cast<float>(std::min(1.0, static_cast<double>(j.done) / j.total)) : 0;
                    c.Box(12, 52, std::max(1.f, actionX - 24), 2, 1, .9f);
                    if (fraction > 0) c.Box(12, 52, (actionX - 24) * fraction, 2, 1, .4f);
                }
                c.Box(actionX, 15, 90, 28, 5, .98f, .85f);
                c.Text(L"打开文件夹", actionX + 4, 17, 82, 24, captionFormat, j.savedPath.empty() ? .7f : .3f, DWRITE_TEXT_ALIGNMENT_CENTER);
                c.Box(w - 58, 15, 50, 28, 5, .98f, .85f);
                c.Text(j.state < State::Completed ? L"取消" : L"删除", w - 56, 17, 46, 24, captionFormat, .3f, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            return;
        }
        if (d.CtlID == Drop) {
            c.Box(1, 1, w - 2, h - 2, 16,
                  selected   ? .94f
                  : hover    ? .985f
                  : disabled ? .96f
                             : 1.f,
                  keyboardFocus ? .4f
                  : hover       ? .68f
                                : .82f);
            c.Box(w / 2 - 23, 19, 46, 46, 12, .94f);
            c.Icon(7, w / 2 - 12, 30, disabled ? .67f : .32f);
            c.Text(L"拖入文件或文件夹", 20, 77, w - 40, 30, section, disabled ? .6f : .21f,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            c.Text(displayedPeer, 20, 111, w - 40, 25, normal, disabled ? .65f : .43f,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            c.Text(disabled ? L"" : L"点击选择文件或文件夹", 20, 146, w - 40, 23, captionFormat, .53f,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        wchar_t text[100]{};
        GetWindowTextW(d.hwndItem, text, 100);
        bool solid = d.CtlID == Accept || (GetParent(d.hwndItem) != window && d.CtlID == 9);
        bool outlined = d.CtlID == Settings || d.CtlID == WebService || d.CtlID == CopyUrl ||
                        GetParent(d.hwndItem) != window;
        float fill = solid      ? (selected ? .12f
                                   : hover  ? .25f
                                            : .19f)
                     : selected ? .89f
                     : hover    ? .92f
                     : outlined ? 1.f
                                : background;
        c.Box(1, 1, w - 2, h - 2, 8, fill, keyboardFocus ? .45f : outlined && !solid ? .86f : -1);
        float ink = disabled ? .68f : solid ? 1.f : .26f;
        int icon = d.CtlID == WebService    ? 8
                   : d.CtlID == PingPeer    ? 9
                   : d.CtlID == NotePeer    ? 10
                   : d.CtlID == Settings    ? 4
                   : d.CtlID == Refresh     ? 6
                   : d.CtlID == ConnectPeer ? 5
                   : d.CtlID == OpenFolder  ? 3
                                            : -1;
        if (icon >= 0) {
            bool side = d.CtlID == ConnectPeer || d.CtlID == OpenFolder;
            float x = d.CtlID == Refresh ? (w - 24) / 2 : side ? 12.f : 13.f;
            c.Icon(icon, x, (h - 24) / 2, ink);
            if (d.CtlID != Refresh)
                c.Text(text, side ? 48.f : 42.f, 0, w - (side ? 55.f : 49.f), h, normal, ink);
        } else
            c.Text(text, 0, 0, w, h, captionFormat, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void SendPaths(const std::vector<fs::path> &files) {
        auto it = std::find_if(shownPeers.begin(), shownPeers.end(), [&](auto &p) { return p.id == peerId; });
        if (it == shownPeers.end()) {
            localNotice = L"未选择在线目标设备";
            Update();
            return;
        }
        engine.SendFiles(*it, files);
        localNotice = L"";
        Update();
    }
    void ChooseFiles() {
        if (!dropEnabled)
            return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"选择文件…");
        AppendMenuW(menu, MF_STRING, 2, L"选择文件夹…");
        RECT bounds{}; GetWindowRect(drop, &bounds);
        UINT selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_CENTERALIGN,
            (bounds.left + bounds.right) / 2, bounds.top + Px(140), 0, window, nullptr);
        DestroyMenu(menu);
        if (!selection) return;
        IFileOpenDialog *picker = nullptr;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))))
            return;
        DWORD options = 0;
        picker->GetOptions(&options);
        picker->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
            (selection == 2 ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        picker->SetTitle(selection == 2 ? L"选择要发送的文件夹" : L"选择要发送的文件");
        if (SUCCEEDED(picker->Show(window))) {
            IShellItemArray *items = nullptr;
            if (SUCCEEDED(picker->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                std::vector<fs::path> paths;
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem *item = nullptr;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &item))) {
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            paths.emplace_back(path);
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                items->Release();
                SendPaths(paths);
            }
        }
        picker->Release();
    }
    void CopyAddress(HWND addressControl) {
        wchar_t text[512]{};
        GetWindowTextW(addressControl, text, 512);
        // Display labels include the adapter name; clipboard contains only the URL.
        if (auto separator = wcsstr(text, L"  ·  "))
            *separator = 0;
        if (OpenClipboard(window)) {
            EmptyClipboard();
            size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
            HGLOBAL value = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (value) {
                void *p = GlobalLock(value);
                if (p) {
                    memcpy(p, text, bytes);
                    GlobalUnlock(value);
                    if (!SetClipboardData(CF_UNICODETEXT, value))
                        GlobalFree(value);
                } else
                    GlobalFree(value);
            }
            CloseClipboard();
            localNotice = L"Web服务地址已复制";
            Update();
        }
    }
    void OnDrop(HDROP value) {
        if (!dropEnabled) {
            DragFinish(value);
            return;
        }
        POINT point{};
        DragQueryPoint(value, &point);
        RECT rect{};
        GetWindowRect(drop, &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&rect), 2);
        if (PtInRect(&rect, point)) {
            UINT count = DragQueryFileW(value, 0xffffffff, nullptr, 0);
            std::vector<fs::path> paths;
            for (UINT i = 0; i < count; ++i) {
                UINT length = DragQueryFileW(value, i, nullptr, 0);
                std::wstring path(length + 1, 0);
                DragQueryFileW(value, i, path.data(), length + 1);
                path.resize(length);
                paths.emplace_back(path);
            }
            SendPaths(paths);
        } else {
            localNotice = L"请将文件拖入矩形传输区域";
            Update();
        }
        DragFinish(value);
    }
    void SettingsDialog();
    void ConnectDialog();
    void WebDialog();
    void NoteDialog();
    void PartialDialog();
    bool ToggleTask(WPARAM = VK_SPACE) { return false; }
    void CancelTask() {
        auto found = std::find_if(shownJobs.begin(), shownJobs.end(), [&](auto &j) { return j.id == selectedTask; });
        if (found == shownJobs.end()) return;
        if (found->folderRow) { for (auto id : found->members) engine.Cancel(id); }
        else engine.Cancel(selectedTask);
        Update();
    }
    void OpenTaskFile() {
        auto found =
            std::find_if(shownJobs.begin(), shownJobs.end(), [&](auto &j) { return j.id == selectedTask; });
        if (found == shownJobs.end() || found->savedPath.empty())
            return;
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(found->savedPath.c_str());
        if (item) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        } else {
            localNotice = L"文件已移除，无法定位";
            Update();
        }
    }
    void DeleteTask() {
        auto found =
            std::find_if(shownJobs.begin(), shownJobs.end(), [&](auto &j) { return j.id == selectedTask; });
        if (found == shownJobs.end() || found->state == State::Queued || found->state == State::Waiting ||
            found->state == State::Running)
            return;
        auto message = std::wstring(found->folderRow ? L"删除此文件夹批次的传输记录及对应文件。\n\n" : L"删除此传输记录及对应文件。\n\n") +
                       (found->savedPath.empty() ? found->name : found->savedPath.wstring());
        if (MessageBoxW(window, message.c_str(), L"删除记录和文件",
                        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
        std::wstring error;
        bool removed = true;
        if (found->folderRow) { for (auto id : found->members) if (!engine.DeleteTransfer(id, true, error)) removed = false; }
        else removed = engine.DeleteTransfer(selectedTask, true, error);
        localNotice = removed ? L"记录和对应文件已删除" : error;
        Update();
    }
    void Command(int id, int code) {
        if (id == Devices && code == LBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(devices, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(shownPeers.size()))
                peerId = shownPeers[sel].id;
            Update();
        }
        if (id == Tasks && code == LBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(tasks, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(shownJobs.size()))
                selectedTask = shownJobs[sel].id;
            Update();
        }
        if (code != BN_CLICKED)
            return;
        switch (id) {
        case Drop:
            ChooseFiles();
            break;
        case Settings:
            SettingsDialog();
            break;
        case Refresh:
            engine.Refresh();
            localNotice = L"已刷新设备列表";
            Update();
            break;
        case ConnectPeer:
            ConnectDialog();
            break;
        case PingPeer:
            for (auto &p : shownPeers) if (p.id == peerId) engine.Ping(p);
            break;
        case NotePeer:
            NoteDialog();
            break;
        case WebService:
            WebDialog();
            break;
        case OpenFolder:
            ShellExecuteW(window, L"open", snapshot.config.receiveDir.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        case Cancel:
            CancelTask();
            break;
        case Clear:
            engine.ClearFinished();
            Update();
            break;
        case Delete:
            DeleteTask();
            break;
        case Partial:
            PartialDialog();
            break;
        case Accept:
            engine.Decide(pending, true);
            Update();
            break;
        case Reject:
            engine.Decide(pending, false);
            Update();
            break;
        default:
            break;
        }
    }
};

struct Modal {
    App &app;
    HWND window = nullptr, name = nullptr, path = nullptr, autoReceive = nullptr, share = nullptr,
         port = nullptr, help = nullptr, maxFiles = nullptr, maxBytes = nullptr, itemList = nullptr;
    bool settings = false, done = false, web = false, note = false, approval = false;
    uint64_t approvalId = 0;
    std::vector<OfferItem> approvalItems;
    std::string noteIp;
    std::vector<D2D1_RECT_F> editFrames;
    explicit Modal(App &a, bool isSettings, bool isWeb = false) : app(a), settings(isSettings), web(isWeb) {}
    HWND Control(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
        if (wcscmp(cls, L"EDIT") == 0) {
            editFrames.push_back(D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(x + w), static_cast<float>(y + h)));
            x += 9;
            y += 5;
            w -= 18;
            h -= 10;
        }
        HWND c = CreateWindowExW(
            0, cls, text, WS_CHILD | WS_VISIBLE | style, app.Px(static_cast<float>(x)),
            app.Px(static_cast<float>(y)), app.Px(static_cast<float>(w)), app.Px(static_cast<float>(h)),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
        SetWindowTheme(c, L"Explorer", nullptr);
        if (wcscmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) != BS_AUTOCHECKBOX) {
            SetWindowLongPtrW(c, GWL_STYLE, (GetWindowLongPtrW(c, GWL_STYLE) & ~BS_TYPEMASK) | BS_OWNERDRAW);
            SetWindowSubclass(c, App::ButtonProc, 1, reinterpret_cast<DWORD_PTR>(&app));
        }
        return c;
    }
    void Paint() {
        PAINTSTRUCT ps{};
        BeginPaint(window, &ps);
        RECT r{};
        GetClientRect(window, &r);
        app.Buffered(ps.hdc, r, 1, [&](ui::Canvas &c, float w, float) {
            for (auto &r : editFrames)
                c.Box(r.left, r.top, r.right - r.left, r.bottom - r.top, 7, 1, .84f);
            c.Line(24,
                   web        ? 184.f
                   : settings ? 444.f : approval ? 420.f
                              : 152.f,
                   w - 24,
                   web        ? 184.f
                   : settings ? 444.f : approval ? 420.f
                              : 152.f,
                   .9f, 1);
        });
        EndPaint(window, &ps);
    }
    std::wstring Value(HWND c) {
        int n = GetWindowTextLengthW(c);
        std::wstring text(static_cast<size_t>(n) + 1, 0);
        GetWindowTextW(c, text.data(), n + 1);
        text.resize(n);
        return text;
    }
    void Create() {
        auto &cfg = app.snapshot.config;
        if (approval) {
            Control(L"STATIC", L"选择要接收的项目；文件夹始终作为一个整体。", 0, 24, 20, 560, 28, 0);
            itemList = Control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 24, 60, 560, 340, 20);
            ListView_SetExtendedListViewStyle(itemList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t *>(L"项目"); column.cx = app.Px(382); ListView_InsertColumn(itemList, 0, &column);
            column.pszText = const_cast<wchar_t *>(L"大小"); column.cx = app.Px(138); ListView_InsertColumn(itemList, 1, &column);
            SendMessageW(itemList, WM_SETREDRAW, FALSE, 0);
            for (size_t i = 0; i < approvalItems.size(); ++i) {
                auto label = (approvalItems[i].folder ? L"文件夹 · " : L"") + approvalItems[i].name;
                LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = label.data();
                ListView_InsertItem(itemList, &item); auto size = FormatBytes(approvalItems[i].total);
                ListView_SetItemText(itemList, item.iItem, 1, size.data());
            }
            SendMessageW(itemList, WM_SETREDRAW, TRUE, 0);
            help = Control(L"STATIC", L"未勾选的项目不会接收。", 0, 24, 437, 260, 30, 0);
            Control(L"BUTTON", L"返回", WS_TABSTOP, 380, 436, 88, 34, 8);
            Control(L"BUTTON", L"接受所选", WS_TABSTOP, 480, 436, 104, 34, 9);
            name = itemList;
        } else if (note) {
            auto label = L"IP " + Wide(noteIp) + L" 的临时备注";
            Control(L"STATIC", label.c_str(), 0, 24, 20, 420, 24, 0);
            name = Control(L"EDIT", app.engine.GetNote(noteIp).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 24, 51, 420, 31, 1);
            SendMessageW(name, EM_SETLIMITTEXT, 48, 0);
            help = Control(L"STATIC", L"仅本客户端可见；结束本轮会话后清空。", 0, 24, 102, 420, 44, 0);
            Control(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 268, 165, 81, 33, 8);
            Control(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, 360, 165, 84, 33, 9);
        } else if (web) {
            Control(L"STATIC", L"服务地址", 0, 24, 20, 200, 24, 0);
            name = Control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 24,
                           53, 542, 150, 1);
            auto addresses = app.snapshot.interfaces;
            if (addresses.empty())
                addresses.push_back({"127.0.0.1", L"本机回环", false});
            for (auto &ip : addresses) {
                auto address = L"http://" + Wide(ip.ip) + L":" + std::to_wstring(app.snapshot.boundPort) +
                               L"/  ·  " + ip.name + (ip.virtualAdapter ? L"（虚拟网卡）" : L"");
                SendMessageW(name, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(address.c_str()));
            }
            SendMessageW(name, CB_SETCURSEL, 0, 0);
            Control(L"STATIC", L"提供局域网内连接设备软件下载和在线web服务。", SS_LEFT, 24, 103, 542, 36, 0);
            help = Control(L"STATIC", cfg.shareSoftware ? L"服务已开启" : L"服务已关闭，可在设置中开启",
                           SS_LEFT, 24, 145, 542, 24, 0);
            Control(L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 24, 200, 76, 34, 8);
            HWND copy = Control(L"BUTTON", L"复制地址", WS_TABSTOP | BS_PUSHBUTTON, 336, 200, 100, 34, 10);
            HWND open = Control(L"BUTTON", L"打开网页", WS_TABSTOP | BS_PUSHBUTTON, 448, 200, 118, 34, 11);
            EnableWindow(copy, cfg.shareSoftware);
            EnableWindow(open, cfg.shareSoftware);
        } else if (settings) {
            Control(L"STATIC", L"设备名称", 0, 24, 20, 130, 22, 0);
            name = Control(L"EDIT", cfg.automaticName ? L"" : cfg.name.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 24, 45, 420, 30, 1);
            SendMessageW(name, EM_SETLIMITTEXT, 64, 0);
            Control(L"STATIC", L"接收目录", 0, 24, 90, 130, 22, 0);
            path = Control(L"EDIT", cfg.receiveDir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 24, 116, 329, 31, 2);
            Control(L"BUTTON", L"选择", WS_TABSTOP | BS_PUSHBUTTON, 363, 116, 81, 31, 3);
            autoReceive = Control(L"BUTTON", L"自动接收在线设备发送的文件", WS_TABSTOP | BS_AUTOCHECKBOX, 24,
                                  163, 420, 28, 4);
            SendMessageW(autoReceive, BM_SETCHECK, cfg.autoReceive ? BST_CHECKED : BST_UNCHECKED, 0);
            share = Control(L"BUTTON", L"启用Web服务", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 199, 420, 28, 5);
            SendMessageW(share, BM_SETCHECK, cfg.shareSoftware ? BST_CHECKED : BST_UNCHECKED, 0);
            Control(L"STATIC", L"TCP 服务端口（重启后生效）", 0, 24, 243, 330, 24, 0);
            port = Control(L"EDIT", std::to_wstring(cfg.port).c_str(),
                           WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 24, 272, 150, 30, 6);
            SendMessageW(port, EM_SETLIMITTEXT, 5, 0);
            Control(L"STATIC", L"单批文件数量（0 = 无限）", 0, 24, 319, 206, 22, 0);
            maxFiles = Control(L"EDIT", std::to_wstring(cfg.maxTransferFiles).c_str(), WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 24, 347, 198, 31, 12);
            Control(L"STATIC", L"单批大小 / MiB（0 = 无限）", 0, 238, 319, 206, 22, 0);
            maxBytes = Control(L"EDIT", std::to_wstring(cfg.maxTransferBytes / 1048576).c_str(), WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 238, 347, 206, 31, 13);
            SendMessageW(maxFiles, EM_SETLIMITTEXT, 20, 0); SendMessageW(maxBytes, EM_SETLIMITTEXT, 14, 0);
            help = Control(L"STATIC", L"上限同时用于发送和接收。关闭自动接收可逐批审批。", SS_LEFT, 24, 394, 420, 42, 0);
            Control(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 268, 457, 81, 33, 8);
            Control(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, 360, 457, 84, 33, 9);
        } else {
            Control(L"STATIC", L"目标设备地址", 0, 24, 20, 350, 24, 0);
            name = Control(L"EDIT", L"192.168.1.12:45871", WS_TABSTOP | ES_AUTOHSCROLL, 24, 51, 420, 31, 1);
            help = Control(L"STATIC", L"输入同一局域网内的 IPv4 地址和端口。", 0, 24, 102, 420, 44, 0);
            Control(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 268, 165, 81, 33, 8);
            Control(L"BUTTON", L"连接", WS_TABSTOP | BS_DEFPUSHBUTTON, 360, 165, 84, 33, 9);
        }
        SetFocus(name);
    }
    void Browse() {
        IFileOpenDialog *picker = nullptr;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))))
            return;
        DWORD flags;
        picker->GetOptions(&flags);
        picker->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(picker->Show(window))) {
            IShellItem *item = nullptr;
            PWSTR value = nullptr;
            if (SUCCEEDED(picker->GetResult(&item))) {
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                    SetWindowTextW(path, value);
                    CoTaskMemFree(value);
                }
                item->Release();
            }
        }
        picker->Release();
    }
    void OpenWeb() {
        if (app.snapshot.config.shareSoftware) {
            auto address = Value(name);
            if (auto separator = address.find(L"  ·  "); separator != std::wstring::npos)
                address.resize(separator);
            ShellExecuteW(window, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    void Save() {
        if (approval) {
            std::string selected(approvalItems.size(), '0');
            for (size_t i = 0; i < selected.size(); ++i) if (ListView_GetCheckState(itemList, static_cast<int>(i))) selected[i] = '1';
            if (selected.find('1') == std::string::npos) { SetWindowTextW(help, L"请至少选择一个项目。"); return; }
            app.engine.DecideItems(approvalId, selected); done = true; return;
        }
        if (note) { app.engine.SetNote(noteIp, Value(name)); InvalidateRect(app.devices, nullptr, FALSE); done = true; return; }
        if (web) {
            OpenWeb();
            return;
        }
        std::wstring error;
        if (settings) {
            Config cfg = app.snapshot.config;
            cfg.name = Value(name);
            cfg.receiveDir = Value(path);
            cfg.autoReceive = SendMessageW(autoReceive, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg.shareSoftware = SendMessageW(share, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!ValidPort(Value(port), cfg.port)) {
                SetWindowTextW(help, L"端口必须为 1024 至 65535。 ");
                return;
            }
            auto parse = [](const std::wstring &value, uint64_t &result) {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
                result = 0; for (auto c : value) { const auto digit = static_cast<uint64_t>(c - L'0'); if (result > (UINT64_MAX - digit) / 10) return false; result = result * 10 + digit; } return true;
            };
            uint64_t mib = 0;
            if (!parse(Value(maxFiles), cfg.maxTransferFiles) || !parse(Value(maxBytes), mib) || mib > UINT64_MAX / 1048576) {
                SetWindowTextW(help, L"上限必须为可表示的非负整数；0 表示无限制。"); return;
            }
            cfg.maxTransferBytes = mib * 1048576;
            if (!app.engine.UpdateConfig(cfg, error)) {
                SetWindowTextW(help, error.c_str());
                return;
            }
            app.localNotice =
                cfg.port != app.snapshot.boundPort ? L"设置已保存，端口变更重启后生效" : L"设置已保存";
            done = true;
        } else {
            SetWindowTextW(help, L"正在连接设备…");
            UpdateWindow(window);
            if (!app.engine.AddPeer(Utf8(Value(name)), error)) {
                SetWindowTextW(help, error.c_str());
                return;
            }
            app.localNotice = L"设备已连接";
            done = true;
        }
    }
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Modal *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Modal *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, msg, w, l);
        switch (msg) {
        case WM_CREATE:
            self->Create();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_COMMAND:
            if (self->web && HIWORD(w) == BN_CLICKED) {
                if (LOWORD(w) == 10)
                    self->app.CopyAddress(self->name);
                if (LOWORD(w) == 11)
                    self->OpenWeb();
            }
            if (LOWORD(w) == 8)
                self->done = true;
            if (LOWORD(w) == 9)
                self->Save();
            if (LOWORD(w) == 3)
                self->Browse();
            return 0;
        case WM_DRAWITEM:
            self->app.DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
            return TRUE;
        case WM_CLOSE:
            self->done = true;
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(w), Muted);
            return reinterpret_cast<LRESULT>(self->app.white);
        case WM_CTLCOLORDLG:
            return reinterpret_cast<LRESULT>(self->app.white);
        default:
            return DefWindowProcW(h, msg, w, l);
        }
    }
    void Run() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"ChuanModal";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = app.white;
        RegisterClassW(&cls);
        int h = web ? 254 : settings ? 522 : approval ? 496 : 232;
        RECT r{0, 0, app.Px(approval ? 608.f : web ? 590.f : 470.f), app.Px(static_cast<float>(h))};
        AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_DLGMODALFRAME,
                                 GetDpiForWindow(app.window));
        RECT owner{};
        GetWindowRect(app.window, &owner);
        window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ChuanModal",
                                 web        ? L"Web服务"
                                 : settings ? L"设置"
                                 : approval ? L"接受部分文件" : note ? L"IP 临时备注" : L"手动连接",
                                 WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 web ? owner.right - (r.right - r.left) - app.Px(32)
                                     : owner.left + (owner.right - owner.left - (r.right - r.left)) / 2,
                                 web ? owner.bottom - (r.bottom - r.top) - app.Px(92)
                                     : owner.top + (owner.bottom - owner.top - (r.bottom - r.top)) / 2,
                                 r.right - r.left, r.bottom - r.top, app.window, nullptr,
                                 GetModuleHandleW(nullptr), this);
        if (!window)
            return;
        EnableWindow(app.window, FALSE);
        ShowWindow(window, SW_SHOW);
        MSG message{};
        while (!done && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                done = true;
                break;
            }
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
                Save();
                continue;
            }
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        DestroyWindow(window);
        EnableWindow(app.window, TRUE);
        SetForegroundWindow(app.window);
        app.Update();
    }
};
void App::PartialDialog() {
    for (auto &job : snapshot.transfers) if (job.id == pending && job.offerItems.size() > 1) {
        Modal dialog(*this, false); dialog.approval = true; dialog.approvalId = pending; dialog.approvalItems = job.offerItems; dialog.Run(); break;
    }
}
void App::NoteDialog() {
    for (auto &p : shownPeers) if (p.id == peerId) { Modal dialog(*this, false); dialog.note = true; dialog.noteIp = p.ip; dialog.Run(); break; }
}
void App::WebDialog() {
    Modal(*this, false, true).Run();
}
void App::SettingsDialog() {
    Modal(*this, true).Run();
}
void App::ConnectDialog() {
    Modal(*this, false).Run();
}

LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        app->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(h, message, w, l);
    switch (message) {
    case WM_CREATE:
        app->Initialize();
        app->Layout();
        return 0;
    case WM_SIZE:
        app->Layout();
        return 0;
    case WM_DPICHANGED:
        app->scale = HIWORD(w) / 96.0f;
        app->Fonts();
        if (app->target)
            app->target->SetDpi(96 * app->scale, 96 * app->scale);
        {
            RECT *r = reinterpret_cast<RECT *>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        app->Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto *m = reinterpret_cast<MINMAXINFO *>(l);
        m->ptMinTrackSize = {app->Px(920), app->Px(660)};
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->Paint();
        return 0;
    case WM_TIMER:
        if (w == 2) {
            app->RenderPing(GetTickCount64() - app->pingStarted);
            return 0;
        }
        app->Update();
        return 0;
    case WM_COMMAND:
        app->Command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_DRAWITEM:
        app->DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = app->Px(80);
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        if (reinterpret_cast<HWND>(l) == app->flash) { SetBkColor(reinterpret_cast<HDC>(w), RGB(210,210,210)); return reinterpret_cast<LRESULT>(app->pingBrush); }
        SetTextColor(reinterpret_cast<HDC>(w), Muted);
        SetBkColor(reinterpret_cast<HDC>(w),
                   reinterpret_cast<HWND>(l) == app->status ? RGB(249, 249, 249) : RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(reinterpret_cast<HWND>(l) == app->status ? app->surface
                                                                                  : app->white);
    }
    case WM_CTLCOLORLISTBOX: {
        return reinterpret_cast<LRESULT>(reinterpret_cast<HWND>(l) == app->devices ? app->gray
                                                                                   : app->surface);
    }
    case WM_DROPFILES:
        app->OnDrop(reinterpret_cast<HDROP>(w));
        return 0;
    case WM_CLOSE:
        if (app->engine.HasActiveTransfers() &&
            MessageBoxW(h, L"当前存在未结束的传输。退出将取消这些任务，并停止局域网服务。", L"退出程序",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        app->engine.Stop();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, message, w, l);
    }
}
void WriteResult(const fs::path &path, const Snapshot &state) {
    if (path.empty())
        return;
    std::ofstream out(path, std::ios::binary);
    for (auto &j : state.transfers)
        out << j.id << '\t' << static_cast<int>(j.state) << '\t' << j.done << '\t' << j.total << '\t'
            << Utf8(j.name) << '\n';
}
} // namespace

#ifndef CHUAN_UI_TEST_BUILD
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::map<std::wstring, std::wstring> options;
    std::vector<fs::path> sends;
    for (int i = 1; i < argc; ++i) {
        std::wstring key = argv[i];
        if (key == L"--send" && i + 1 < argc)
            sends.emplace_back(argv[++i]);
        else if (key.starts_with(L"--")) {
            std::wstring value = L"1";
            if (i + 1 < argc && !std::wstring(argv[i + 1]).starts_with(L"--"))
                value = argv[++i];
            options[key] = value;
        }
    }
    LocalFree(argv);
    Config cfg = LoadConfig(options.contains(L"--data-dir") ? fs::path(options[L"--data-dir"]) : fs::path{});
    unsigned short p;
    if (options.contains(L"--port")) {
        if (!ValidPort(options[L"--port"], p)) {
            CoUninitialize();
            return 2;
        }
        cfg.port = p;
    }
    if (options.contains(L"--discovery-port")) {
        if (!ValidPort(options[L"--discovery-port"], p)) {
            CoUninitialize();
            return 2;
        }
        cfg.discoveryPort = p;
    }
    if (options.contains(L"--name"))
        cfg.name = options[L"--name"];
    if (options.contains(L"--receive-dir"))
        cfg.receiveDir = fs::absolute(options[L"--receive-dir"]);
    if (options.contains(L"--no-auto"))
        cfg.autoReceive = false;
    if (options.contains(L"--no-share"))
        cfg.shareSoftware = false;
    bool headless = options.contains(L"--headless") || options.contains(L"--send-to");
    HANDLE mutex = nullptr;
    if (!headless) {
        std::wstring key = L"Local\\Chuan-" + cfg.id;
        mutex = CreateMutexW(nullptr, FALSE, key.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"当前用户的传输程序已运行。", L"局域网传输", MB_OK | MB_ICONINFORMATION);
            if (mutex)
                CloseHandle(mutex);
            CoUninitialize();
            return 0;
        }
    }
    Engine engine(cfg);
    std::wstring error;
    if (!engine.Start(error)) {
        if (!headless)
            MessageBoxW(nullptr, error.c_str(), L"启动失败", MB_OK | MB_ICONERROR);
        else
            std::cerr << Utf8(error) << '\n';
        if (mutex)
            CloseHandle(mutex);
        CoUninitialize();
        return 3;
    }
    const auto startupNotice = engine.GetSnapshot().notice;
    if (!headless && !startupNotice.empty())
        MessageBoxW(nullptr, startupNotice.c_str(), L"启动提示", MB_OK | MB_ICONWARNING);
    else if (headless && !startupNotice.empty())
        std::cerr << Utf8(startupNotice) << '\n';
    if (headless) {
        int code = 0;
        if (options.contains(L"--send-to")) {
            if (sends.empty() || !engine.AddPeer(Utf8(options[L"--send-to"]), error))
                code = 4;
            else {
                auto state = engine.GetSnapshot();
                if (state.peers.empty())
                    code = 4;
                else {
                    engine.SendFiles(state.peers[0], sends);
                    if (!engine.WaitForIdle(std::chrono::seconds(180)))
                        code = 5;
                    auto final = engine.GetSnapshot();
                    if (final.transfers.empty() ||
                        std::any_of(final.transfers.begin(), final.transfers.end(),
                                    [](auto &j) { return j.state != State::Completed; }))
                        code = 6;
                    WriteResult(options.contains(L"--result-file") ? fs::path(options[L"--result-file"])
                                                                   : fs::path{},
                                final);
                }
            }
        } else {
            uint64_t seconds = 60;
            try {
                if (options.contains(L"--run-seconds"))
                    seconds = std::stoull(options[L"--run-seconds"]);
            } catch (...) {
                code = 2;
            }
            auto end = GetTickCount64() + std::min<uint64_t>(seconds, 86400) * 1000;
            while (!code && GetTickCount64() < end)
                Sleep(100);
        }
        engine.Stop();
        CoUninitialize();
        return code;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    App app(engine);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"ChuanMain";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                              LR_DEFAULTCOLOR));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                LR_DEFAULTCOLOR));
    cls.hbrBackground = app.white;
    RegisterClassExW(&cls);
    UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(1040, dpi, 96), MulDiv(700, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"传 · 局域网文件传输",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, &app);
    if (!window) {
        engine.Stop();
        if (mutex)
            CloseHandle(mutex);
        CoUninitialize();
        return 7;
    }
    DWORD corner = 2;
    COLORREF caption = RGB(249, 249, 249), border = RGB(226, 226, 226);
    DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 34, &border, sizeof(border));
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (mutex)
        CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
#endif
