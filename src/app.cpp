#include "third_party/qrcodegen/qrcodegen.hpp"
#include "core.h"
#include "ui.h"
#include "transfer_view.h"
#include "approval_view.h"
#include "updater.h"
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
              OpenFolder = 112, WebService = 113, Delete = 114, PingPeer = 115, NotePeer = 116, Partial = 117, SelectFiles = 118, SelectFolder = 119, ToggleAuto = 120,
              AllTasks = 121, SentTasks = 122, ReceivedTasks = 123, PendingRequests = 124, SelectAllRequests = 125, ShowQr = 126;
constexpr COLORREF Ink = RGB(23, 27, 36), Muted = RGB(107, 116, 133), Line = RGB(220, 226, 234);
const D2D1_COLOR_F Accent = D2D1::ColorF(0x2164F3), AccentLight = D2D1::ColorF(0xDFE9FC),
    Success = D2D1::ColorF(0x08A344), PendingSurface = D2D1::ColorF(0xFFF8E9),
    TextInk = D2D1::ColorF(0x171B24), TextMuted = D2D1::ColorF(0x6B7485),
    SoftBorder = D2D1::ColorF(0xDCE2EA), SideSurface = D2D1::ColorF(0xF1F4F7);
RECT WindowWorkArea(HWND window = nullptr) {
    POINT cursor{};
    GetCursorPos(&cursor);
    const auto monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    return area;
}
RECT DefaultWindowBounds(const RECT &workArea) {
    const LONG width = (workArea.right - workArea.left) * 2 / 3;
    const LONG height = (workArea.bottom - workArea.top) * 2 / 3;
    const LONG left = workArea.left + (workArea.right - workArea.left - width) / 2;
    const LONG top = workArea.top + (workArea.bottom - workArea.top - height) / 2;
    return {left, top, left + width, top + height};
}
struct App {
    Engine &engine;
    update::Manager updater;
    bool settingsUpdateDot = false;
    HWND window = nullptr, devices = nullptr, tasks = nullptr, drop = nullptr, request = nullptr,
         status = nullptr, accept = nullptr, reject = nullptr, cancel = nullptr, partial = nullptr;
    HWND settingsButton = nullptr, refreshButton = nullptr, connectButton = nullptr, clearButton = nullptr,
         folderButton = nullptr, webButton = nullptr, deleteButton = nullptr, qrButton = nullptr;
    HWND pingButton = nullptr, noteButton = nullptr, flash = nullptr;
    HWND requestList = nullptr, selectAllRequests = nullptr;
    HDWP layoutBatch = nullptr;
    ApprovalView approvals;
    uint64_t displayedPing = 0;
    uint64_t pingStarted = 0;
    bool dropEnabled = false;
    HFONT font = nullptr, smallFont = nullptr, titleFont = nullptr;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), gray = CreateSolidBrush(RGB(241, 244, 247)), pendingBrush = CreateSolidBrush(RGB(255, 248, 233)),
           surface = CreateSolidBrush(RGB(249, 249, 249)), pingBrush = CreateSolidBrush(RGB(210, 210, 210));
    HBRUSH footerBrush = CreateSolidBrush(RGB(249, 250, 252));
    ID2D1Factory *factory = nullptr;
    ID2D1HwndRenderTarget *target = nullptr;
    ID2D1DCRenderTarget *controlTarget = nullptr;
    IDWriteFactory *write = nullptr;
    IDWriteTextFormat *normal = nullptr, *captionFormat = nullptr, *heading = nullptr, *section = nullptr;
    IDWriteTextFormat *strongFormat = nullptr, *brandFormat = nullptr, *heroFormat = nullptr;
    Snapshot snapshot;
    std::wstring peerId, taskKey;
    uint64_t selectedTask = 0, pending = 0;
    HWND filesButton = nullptr, sendFolderButton = nullptr, autoButton = nullptr;
    HWND webAddress = nullptr, webCopyButton = nullptr, webOpenButton = nullptr;
    HWND allButton = nullptr, sentButton = nullptr, receivedButton = nullptr;
    int taskFilter = 0;
    int sidebarWidth = 276, mainLeft = 300, dropTop = 154, dropHeight = 224;
    int requestTop = 394, requestHeight = 84, taskRowHeight = 80, peerRowHeight = 72;
    bool pendingPartial = false;
    float scale = 1;
    int width = 980, height = 700, jobTop = 345;
    std::vector<Peer> shownPeers;
    std::vector<TransferRow> shownJobs;
    std::vector<Transfer> pendingJobs;
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
        if (strongFormat) strongFormat->Release();
        if (brandFormat) brandFormat->Release();
        if (heroFormat) heroFormat->Release();
        if (write)
            write->Release();
        if (factory)
            factory->Release();
        DeleteObject(font);
        DeleteObject(smallFont);
        DeleteObject(titleFont);
        DeleteObject(white);
        DeleteObject(gray);
        DeleteObject(pendingBrush);
        DeleteObject(surface);
        DeleteObject(pingBrush);
        DeleteObject(footerBrush);
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
        font = MakeFont(16);
        smallFont = MakeFont(14);
        titleFont = MakeFont(24, FW_BOLD);
        EnumChildWindows(
            window,
            [](HWND h, LPARAM p) {
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(reinterpret_cast<App *>(p)->font), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
    }
    static LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
        if (msg == WM_SETCURSOR && GetDlgCtrlID(h) == OpenFolder) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
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
                if (!app->ToggleTask(w) && w == VK_RETURN) app->OpenOrRetryTask();
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
                if (app->shownJobs[LOWORD(row)].childRow) { app->ToggleTask();return result; }
                if (x >= bounds.right - app->Px(58)) {
                    auto &job = app->shownJobs[LOWORD(row)];
                    if (job.state < State::Completed) app->CancelTask();
                    else app->DeleteTask();
                } else if (x >= bounds.right - app->Px(154)) app->OpenOrRetryTask();
                else app->ToggleTask();
            }
        }
        return result;
    }
    void RefreshPending() {
        approvals.Update(pendingJobs);
        const auto selected=SendMessageW(requestList,LB_GETCURSEL,0,0),top=SendMessageW(requestList,LB_GETTOPINDEX,0,0);
        SendMessageW(requestList,WM_SETREDRAW,FALSE,0);SendMessageW(requestList,LB_RESETCONTENT,0,0);
        for(const auto &row:approvals.rows)SendMessageW(requestList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.name.c_str()));
        SendMessageW(requestList,LB_SETCURSEL,std::min<LRESULT>(selected,static_cast<LRESULT>(approvals.rows.size())-1),0);
        SendMessageW(requestList,LB_SETTOPINDEX,top,0);SendMessageW(requestList,WM_SETREDRAW,TRUE,0);
        SetTextIfChanged(request,L"接收申请 ("+std::to_wstring(approvals.leaves.size())+L")");
        EnableIfChanged(accept,approvals.Selected()>0);EnableIfChanged(reject,approvals.Selected()>0);
        InvalidateRect(requestList,nullptr,FALSE);InvalidateRect(selectAllRequests,nullptr,FALSE);
    }
    void ToggleRequest(size_t index,bool checkbox,WPARAM key=0) {
        if(index>=approvals.rows.size())return;const auto row=approvals.rows[index];
        if(row.sender&&!checkbox&&key!=VK_SPACE) {
            if(key==VK_LEFT)approvals.collapsed.insert(row.peer);
            else if(key==VK_RIGHT)approvals.collapsed.erase(row.peer);
            else if(!approvals.collapsed.erase(row.peer))approvals.collapsed.insert(row.peer);
        } else approvals.Toggle(row);
        RefreshPending();Layout();
    }
    static LRESULT CALLBACK RequestProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto *app=reinterpret_cast<App *>(data);
        if(msg==WM_KEYDOWN&&(w==VK_SPACE||w==VK_RETURN||w==VK_LEFT||w==VK_RIGHT)) {
            const auto index=SendMessageW(h,LB_GETCURSEL,0,0);if(index>=0)app->ToggleRequest(static_cast<size_t>(index),w==VK_SPACE,w);return 0;
        }
        const auto result=DefSubclassProc(h,msg,w,l);
        if(msg==WM_LBUTTONUP) { const auto row=SendMessageW(h,LB_ITEMFROMPOINT,0,l);RECT bounds{};GetClientRect(h,&bounds);
            if(!HIWORD(row))app->ToggleRequest(LOWORD(row),GET_X_LPARAM(l)>=bounds.right-app->Px(45));
        }
        return result;
    }
    HWND Button(const wchar_t *text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(h, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }
    HWND Label(const wchar_t *text) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    void Initialize() {
        scale = GetDpiForWindow(window) / 96.0f;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(&write));
        if (write) {
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"zh-CN",
                                    &normal);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN",
                                    &captionFormat);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 32, L"zh-CN",
                                    &heading);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"zh-CN",
                                    &section);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"zh-CN", &strongFormat);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40, L"zh-CN", &brandFormat);
            write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24, L"zh-CN", &heroFormat);
            for (auto *format : {normal, captionFormat, heading, section, strongFormat, brandFormat, heroFormat}) {
                IDWriteInlineObject *ellipsis = nullptr;
                if (format && SUCCEEDED(write->CreateEllipsisTrimmingSign(format, &ellipsis))) {
                    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    format->SetTrimming(&trimming, ellipsis);
                    ellipsis->Release();
                }
            }
        }
        devices = CreateWindowExW(0, L"LISTBOX", L"附近设备",
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                      LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Devices)),
                                  GetModuleHandleW(nullptr), nullptr);
        tasks = CreateWindowExW(0, L"LISTBOX", L"传输状态",
                                WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Tasks)),
                                GetModuleHandleW(nullptr), nullptr);
        requestList = CreateWindowExW(0, L"LISTBOX", L"待确认的接收申请",
                                WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PendingRequests)),
                                GetModuleHandleW(nullptr), nullptr);
        drop = Button(L"将文件拖入此区域发送", Drop);
        filesButton = Button(L"选择文件", SelectFiles);
        sendFolderButton = Button(L"选择文件夹", SelectFolder);
        autoButton = Button(L"自动接收", ToggleAuto);
        allButton = Button(L"全部", AllTasks);
        sentButton = Button(L"发送", SentTasks);
        receivedButton = Button(L"接收", ReceivedTasks);
        settingsButton = Button(L"设置", Settings);
        refreshButton = Button(L"刷新", Refresh);
        connectButton = Button(L"手动连接", ConnectPeer);
        webButton = Button(L"Web 服务", WebService);
        qrButton = Button(L"二维码", ShowQr);
        webAddress = Label(L"");
        webCopyButton = Button(L"复制", CopyUrl);
        webOpenButton = Button(L"打开", OpenUrl);
        clearButton = Button(L"清除已结束", Clear);
        cancel = Button(L"取消传输", Cancel);
        deleteButton = Button(L"删除", Delete);
        pingButton = Button(L"Ping", PingPeer);
        noteButton = Button(L"IP 备注", NotePeer);
        SetWindowSubclass(tasks, TaskProc, 2, reinterpret_cast<DWORD_PTR>(this));
        accept = Button(L"接收", Accept);
        reject = Button(L"拒绝", Reject);
        partial = Button(L"接受部分", Partial);
        selectAllRequests = Button(L"全选", SelectAllRequests);
        SetWindowSubclass(requestList, RequestProc, 3, reinterpret_cast<DWORD_PTR>(this));
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
        const auto flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW;
        if(layoutBatch)layoutBatch=DeferWindowPos(layoutBatch,h,nullptr,Px(float(x)),Px(float(y)),Px(float(std::max(0,w))),Px(float(std::max(0,v))),flags);
        else SetWindowPos(h,nullptr,Px(float(x)),Px(float(y)),Px(float(std::max(0,w))),Px(float(std::max(0,v))),flags);
    }
    void Layout() {
        layoutBatch=BeginDeferWindowPos(32);
        RECT r{};
        GetClientRect(window, &r);
        width = static_cast<int>((r.right - r.left) / scale);
        height = static_cast<int>((r.bottom - r.top) / scale);
        sidebarWidth = width >= 1160 ? 276 : 240;
        mainLeft = sidebarWidth + 24;
        const int left = mainLeft, right = width - 24, center = (left + right) / 2;
        const bool compact = height < 720;
        peerRowHeight = compact ? 64 : 72;
        Move(devices, 12, 164, sidebarWidth - 24, std::max(peerRowHeight, (height - (compact ? 236 : 278) - 164) / peerRowHeight * peerRowHeight));
        SendMessageW(devices, LB_SETITEMHEIGHT, 0, Px(shownPeers.empty() ? 60.f : static_cast<float>(peerRowHeight)));
        Move(refreshButton, sidebarWidth - 78, 121, 36, 36);
        Move(pingButton, 20, height - 208, (sidebarWidth - 48) / 2, 42);
        Move(noteButton, sidebarWidth / 2 + 4, height - 208, (sidebarWidth - 48) / 2, 42);
        Move(connectButton, 20, height - 154, sidebarWidth - 40, 36);
        Move(settingsButton, right - 92, 22, 92, 38);
        Move(autoButton, right - 258, 22, 152, 38);
        dropHeight = compact ? 132 : height < 780 ? 184 : 224;
        if (pending) dropHeight = std::min(dropHeight, compact ? 88 : 160);
        const int requestRowHeight = compact ? 44 : 52;
        const int maxRequestRows = std::max(1,(height-94-(width>=1160?80:100)-(dropTop+dropHeight+16)-(compact?48:52)-(compact?50:64))/requestRowHeight);
        const int requestRows = static_cast<int>(std::clamp<size_t>(approvals.rows.size(), 1, std::min(5,maxRequestRows)));
        requestHeight = (compact ? 48 : 52) + requestRowHeight * requestRows;
        requestTop = dropTop + dropHeight + 16;
        Move(drop, left, dropTop, right - left, dropHeight);
        Move(filesButton, center - 144, dropTop + dropHeight - (pending ? 54 : 65), 136, pending ? 40 : 44);
        Move(sendFolderButton, center + 8, dropTop + dropHeight - (pending ? 54 : 65), 136, pending ? 40 : 44);
        jobTop = pending ? requestTop + requestHeight + (compact ? 50 : 64) : dropTop + dropHeight + 64;
        Move(request, left + 16, requestTop + 12, std::max(100, right - left - 350), 26);
        Move(reject, right - 212, requestTop + (compact ? 4 : 6), 84, compact ? 32 : 36);
        Move(selectAllRequests,right-300,requestTop+(compact?4:6),80,compact?32:36);
        Move(accept, right - 112, requestTop + (compact ? 4 : 6), 100, compact ? 32 : 36);
        Move(requestList, left + 12, requestTop + (compact ? 40 : 44), right - left - 24, requestRows * requestRowHeight);
        SendMessageW(requestList, LB_SETITEMHEIGHT, 0, Px(static_cast<float>(requestRowHeight)));
        ShowWindow(request, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(requestList, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(accept, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(reject, pending ? SW_SHOW : SW_HIDE);
        ShowWindow(partial,SW_HIDE);
        ShowWindow(selectAllRequests,pending?SW_SHOW:SW_HIDE);
        ShowWindow(deleteButton, SW_HIDE);
        const int filterWidth = width >= 1160 ? 96 : 60;
        Move(allButton, left + 112, jobTop - 43, filterWidth, 34);
        Move(sentButton, left + 120 + filterWidth, jobTop - 43, filterWidth, 34);
        Move(receivedButton, left + 128 + filterWidth * 2, jobTop - 43, filterWidth, 34);
        Move(clearButton, right - 134, jobTop - 43, 134, 34);
        ShowWindow(cancel, SW_HIDE);
        taskRowHeight = width >= 1160 ? 80 : 100;
        Move(tasks, left, jobTop, right - left, std::max(taskRowHeight, (height - jobTop - 94) / taskRowHeight * taskRowHeight));
        TaskHeights();
        const bool showWebAddress = width >= 1160;
        Move(folderButton, left + 8, height - 85, right - left - (showWebAddress ? 544 : 220), 40);
        Move(webButton, right - (showWebAddress ? 524 : 200), height - 85, 124, 40);
        Move(qrButton, right - (showWebAddress ? 184 : 70), height - 83, showWebAddress ? 58 : 70, 36);
        Move(webAddress, right - 392, height - 78, 200, 26);
        Move(webCopyButton, right - 120, height - 83, 54, 36);
        Move(webOpenButton, right - 60, height - 83, 54, 36);
        ShowWindow(webAddress, showWebAddress ? SW_SHOW : SW_HIDE);
        ShowWindow(webCopyButton, showWebAddress ? SW_SHOW : SW_HIDE);
        ShowWindow(webOpenButton, showWebAddress ? SW_SHOW : SW_HIDE);
        SendMessageW(webAddress, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
        SendMessageW(request, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
        Move(status, 20, height - 29, width - 40, 22);
        SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
        if(layoutBatch)EndDeferWindowPos(layoutBatch);layoutBatch=nullptr;
        for(HWND button:{filesButton,sendFolderButton})SetWindowPos(button,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW);
        if (target) target->Resize(D2D1::SizeU(static_cast<UINT32>(r.right), static_cast<UINT32>(r.bottom)));
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void Update(const Snapshot *sample=nullptr) {
        const bool unread = updater.Get().settingsUnread;
        if (unread != settingsUpdateDot) { settingsUpdateDot = unread; InvalidateRect(settingsButton, nullptr, FALSE); }
        auto oldName = snapshot.config.name;
        const bool oldAutoReceive = snapshot.config.autoReceive;
        snapshot = sample?*sample:engine.GetSnapshot();
        const auto summaryRows = TransferRows(snapshot.transfers, {});
        const auto receivedCount = std::count_if(summaryRows.begin(), summaryRows.end(), [](const auto &job) { return job.receiving; });
        SetTextIfChanged(allButton, width >= 1160 ? L"全部 (" + std::to_wstring(summaryRows.size()) + L")" : L"全部");
        SetTextIfChanged(sentButton, width >= 1160 ? L"发送 (" + std::to_wstring(summaryRows.size() - receivedCount) + L")" : L"发送");
        SetTextIfChanged(receivedButton, width >= 1160 ? L"接收 (" + std::to_wstring(receivedCount) + L")" : L"接收");
        SetTextIfChanged(webAddress, snapshot.config.shareSoftware ? L"http://" + Wide(snapshot.addresses.empty() ? "127.0.0.1" : snapshot.addresses.front()) + L":" +
                         std::to_wstring(snapshot.boundPort ? snapshot.boundPort : snapshot.config.port) + L"/" : L"Web 服务未启用");
        SetTextIfChanged(folderButton, L"接收目录：" + snapshot.config.receiveDir.wstring() + L" · 查看 / 修改");
        EnableIfChanged(qrButton, snapshot.config.shareSoftware);
        EnableIfChanged(webCopyButton, snapshot.config.shareSoftware);
        EnableIfChanged(webOpenButton, snapshot.config.shareSoftware);
        if (oldAutoReceive != snapshot.config.autoReceive) InvalidateRect(autoButton, nullptr, FALSE);
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
                    shownPeers[i].ip != snapshot.peers[i].ip || shownPeers[i].port != snapshot.peers[i].port || shownPeers[i].platform != snapshot.peers[i].platform)
                    changed = true;
        if (changed) {
            shownPeers = snapshot.peers;
            SendMessageW(devices, WM_SETREDRAW, FALSE, 0);
            SendMessageW(devices, LB_RESETCONTENT, 0, 0);
            for (auto &p : shownPeers) {
                std::wstring label = p.name + L" · " + PeerLabel(p) + L" · " + Wide(p.ip) + L":" + std::to_wstring(p.port);
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
        EnableIfChanged(filesButton, canSend);
        EnableIfChanged(sendFolderButton, canSend);
        EnableIfChanged(pingButton, canSend);
        EnableIfChanged(noteButton, canSend);
        if (dropEnabled != canSend) {
            dropEnabled = canSend;
            DragAcceptFiles(window, canSend);
        }
        if (SendMessageW(devices, LB_GETCURSEL, 0, 0) != selected)
            SendMessageW(devices, LB_SETCURSEL, selected, 0);
        auto oldJobs = shownJobs;
        auto visibleTransfers = snapshot.transfers;
        if (taskFilter) std::erase_if(visibleTransfers, [&](const auto &job) { return job.receiving != (taskFilter == 2); });
        std::map<std::string, bool> activeGroups;
        for (const auto &row : summaryRows) if (row.folderRow) activeGroups[row.group] = row.state < State::Completed;
        const auto isActive = [&](const auto &job) {
            const auto group = activeGroups.find(TransferGroup(job));
            return group == activeGroups.end() ? job.state < State::Completed : group->second;
        };
        // 文件夹按批次状态排序，避免成员文件完成时改变批次位置。
        std::stable_sort(visibleTransfers.begin(), visibleTransfers.end(), [&](const auto &a, const auto &b) {
            return isActive(a) > isActive(b);
        });
        shownJobs = TransferRows(visibleTransfers, expandedFolders);
        bool content = oldJobs.size() != shownJobs.size();
        if (!content)
            for (size_t i = 0; i < oldJobs.size(); ++i)
                if (oldJobs[i].id != shownJobs[i].id || oldJobs[i].group != shownJobs[i].group ||
                    oldJobs[i].treePath != shownJobs[i].treePath || oldJobs[i].childRow != shownJobs[i].childRow ||
                    oldJobs[i].directoryRow != shownJobs[i].directoryRow || oldJobs[i].folderRow != shownJobs[i].folderRow)
                    content = true;
        int top = static_cast<int>(SendMessageW(tasks, LB_GETTOPINDEX, 0, 0));
        int sel = -1;
        if (content) {
            SendMessageW(tasks, WM_SETREDRAW, FALSE, 0);
            SendMessageW(tasks, LB_RESETCONTENT, 0, 0);
            for (auto &j : shownJobs) {
                std::wstring label = (j.folderRow ? L"文件夹 " : L"") + j.name + L" · " + TransferAction(j) + L" · " + j.peer;
                SendMessageW(tasks, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            if (shownJobs.empty())
                SendMessageW(tasks, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"暂无传输任务"));
            TaskHeights();
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
        if (content) {
            SendMessageW(tasks, LB_SETTOPINDEX, top, 0);
            SendMessageW(tasks, WM_SETREDRAW, TRUE, 0);
        }
        const auto oldPending=pending;auto oldRequests=std::move(pendingJobs);pendingJobs.clear();
        for(const auto &job:snapshot.transfers)if(job.receiving&&job.state==State::Waiting&&job.awaitingApproval)pendingJobs.push_back(job);
        pending=pendingJobs.empty()?0:pendingJobs.front().id;
        bool requestsChanged=oldRequests.size()!=pendingJobs.size();
        if(!requestsChanged)for(size_t i=0;i<pendingJobs.size();++i)if(pendingJobs[i].id!=oldRequests[i].id||pendingJobs[i].peer!=oldRequests[i].peer||pendingJobs[i].offerSelection!=oldRequests[i].offerSelection||pendingJobs[i].offerItems.size()!=oldRequests[i].offerItems.size())requestsChanged=true;
        if(requestsChanged){RefreshPending();Layout();}else if(oldPending!=pending)Layout();
        bool active =
            sel >= 0 && (shownJobs[sel].state == State::Queued || shownJobs[sel].state == State::Waiting ||
                         shownJobs[sel].state == State::Running);
        EnableIfChanged(cancel, active);
        EnableIfChanged(deleteButton, sel >= 0 && !active);
        EnableIfChanged(clearButton, std::any_of(snapshot.transfers.begin(), snapshot.transfers.end(), [](const auto &j) {
                            return j.state == State::Completed || j.state == State::Failed ||
                                   j.state == State::Cancelled || j.state == State::Rejected;
                        }));
        std::wstring state = snapshot.addresses.empty()
                                 ? L"未检测到局域网地址"
                                 : L"本机：" + snapshot.config.name + L" · " + Wide(snapshot.addresses[0]);
        state += snapshot.config.autoReceive ? L"   自动接收：开" : L"   自动接收：关";
        state += L"   版本 " + std::wstring(Version);
        if (!localNotice.empty())
            state += L"   " + localNotice;
        else if (!snapshot.notice.empty())
            state += L"   " + snapshot.notice;
        SetTextIfChanged(status, state);
        std::wstring destination = L"请先选择在线设备";
        for (auto &p : shownPeers)
            if (p.id == peerId)
                destination = L"发送至 " + p.name;
        if (destination != displayedPeer) {
            displayedPeer = destination;
            InvalidateRect(drop, nullptr, FALSE);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (changed) {
            SendMessageW(devices, LB_SETITEMHEIGHT, 0, Px(shownPeers.empty() ? 60.f : static_cast<float>(peerRowHeight)));
            InvalidateRect(devices, nullptr, FALSE);
        }
        if (content) {
            InvalidateRect(tasks, nullptr, FALSE);
        } else {
            for (size_t i = 0; i < shownJobs.size(); ++i) {
                auto &a = oldJobs[i];
                auto &b = shownJobs[i];
                if (a.done != b.done || a.total != b.total || a.speed != b.speed || a.error != b.error ||
                    a.name != b.name || a.peer != b.peer || a.state != b.state || a.activity != b.activity ||
                    a.completedFiles != b.completedFiles || a.failedFiles != b.failedFiles ||
                    a.cancelledFiles != b.cancelledFiles || a.skippedFiles != b.skippedFiles ||
                    a.folderCount != b.folderCount || a.awaitingApproval != b.awaitingApproval ||
                    a.savedPath != b.savedPath || a.canRetry != b.canRetry || a.skipped != b.skipped ||
                    a.currentFile != b.currentFile || a.currentAction != b.currentAction || a.stageSummary != b.stageSummary ||
                    a.currentDone!=b.currentDone||a.currentTotal!=b.currentTotal||a.currentCount!=b.currentCount||a.currentTransferring!=b.currentTransferring||a.transferringFiles!=b.transferringFiles) {
                    RECT row{};
                    SendMessageW(tasks, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&row));
                    InvalidateRect(tasks, &row, FALSE);
                }
            }
        }
        if (changed || content || requestsChanged || oldName != snapshot.config.name)
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
    void DrawBackground(ui::Canvas &c) {
        const float left = static_cast<float>(mainLeft), right = static_cast<float>(width - 24);
        c.BoxColor(0, 0, static_cast<float>(sidebarWidth), static_cast<float>(height - 38), 0, SideSurface);
        c.Line(static_cast<float>(sidebarWidth), 0, static_cast<float>(sidebarWidth), static_cast<float>(height - 38), .89f, 1);
        c.Line(0, static_cast<float>(height - 38), static_cast<float>(width), static_cast<float>(height - 38), .90f, 1);
        c.Box(28, 30, 44, 44, 11, .19f); c.Icon(0, 36, 38, 1, 1.16f);
        c.TextColor(L"传", 86, 23, 110, 57, brandFormat, TextInk);
        c.TextColor(L"局域网文件传输", 28, 79, 222, 28, normal, TextMuted);
        c.TextColor(L"附近设备", 24, 122, 150, 33, section, TextInk);
        c.TextColor(std::to_wstring(shownPeers.size()), static_cast<float>(sidebarWidth - 36), 125, 24, 26, captionFormat, TextMuted);
        const float toolsLine = static_cast<float>(height - (height < 720 ? 224 : 262));
        c.Line(20, toolsLine, static_cast<float>(sidebarWidth - 20), toolsLine, .86f, 1);
        if (height >= 720) c.TextColor(L"设备工具", 24, static_cast<float>(height - 250), 150, 28, normal, TextMuted);
        c.Line(20, static_cast<float>(height - 160), static_cast<float>(sidebarWidth - 20), static_cast<float>(height - 160), .86f, 1);
        c.Line(20, static_cast<float>(height - 111), static_cast<float>(sidebarWidth - 20), static_cast<float>(height - 111), .86f, 1);
        c.Icon(1, 26, static_cast<float>(height - 93), .3f, 1.3f);
        c.TextColor(snapshot.config.name, 70, static_cast<float>(height - 96), static_cast<float>(sidebarWidth - 86), 28, strongFormat, TextInk);
        c.TextColor(snapshot.addresses.empty() ? L"未检测到局域网地址" : Wide(snapshot.addresses.front()),
                    70, static_cast<float>(height - 70), static_cast<float>(sidebarWidth - 86), 22, captionFormat, TextMuted);
        c.TextColor(L"文件传输", left, 18, 250, 46, heading, TextInk);
        c.BoxColor(left, 76, right - left, 62, 7, SideSurface);
        c.Icon(1, left + 18, 91, .30f, 1.3f);
        std::wstring name = L"请先选择在线设备", address;
        for (const auto &peer : shownPeers) if (peer.id == peerId) { name = peer.name; address = Wide(peer.ip); }
        c.TextColor(L"目标设备： " + name, left + 64, 80, right - left - 84, 31, strongFormat, TextInk);
        c.TextColor(address, left + 156, 109, right - left - 180, 22, captionFormat, TextMuted);
        if (pending) {
            c.BoxColor(left, static_cast<float>(requestTop), right - left, static_cast<float>(requestHeight), 7, PendingSurface, D2D1::ColorF(0xF4CE77));
        }
        c.TextColor(L"本次传输", left, static_cast<float>(jobTop - 44), 110, 34, section, TextInk);
        c.BoxColor(left, static_cast<float>(height - 89), right - left, 47, 7, D2D1::ColorF(0xF9FAFC), SoftBorder);
    }
    void Paint() {
        ++paintCount;
        PAINTSTRUCT ps{}; BeginPaint(window, &ps);
        if (!factory) { FillRect(ps.hdc, &ps.rcPaint, white); EndPaint(window, &ps); return; }
        if (!target) {
            RECT r{}; GetClientRect(window, &r);
            factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(r.right, r.bottom)), &target);
            if (target) target->SetDpi(96 * scale, 96 * scale);
        }
        if (target) {
            target->BeginDraw(); target->Clear(D2D1::ColorF(1.f, 1.f, 1.f));
            ui::Canvas c(target);
            if (c.brush) DrawBackground(c);
            if (target->EndDraw() == D2DERR_RECREATE_TARGET) DiscardTarget();
        }
        EndPaint(window, &ps);
    }
    void DrawItem(const DRAWITEMSTRUCT &d) {
        if (!factory || !write)
            return;
        ++controlPaintCount;
        bool side =
            d.CtlID == Devices || d.CtlID == Refresh || d.CtlID == ConnectPeer || d.CtlID == PingPeer || d.CtlID == NotePeer;
        bool modal = GetParent(d.hwndItem) != window;
        float background = modal || d.CtlID == CopyUrl || d.CtlID == OpenUrl ? 1.f : side ? .963f : 1.f;
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
    void DrawControl(ui::Canvas &c, const DRAWITEMSTRUCT &d, float w, float h, float) {
        bool selected = (d.itemState & ODS_SELECTED) != 0;
        bool disabled = (d.itemState & ODS_DISABLED) != 0;
        bool hover = GetPropW(d.hwndItem, L"ChuanHover") != nullptr;
        bool keyboardFocus = (d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT);
        if(d.CtlID==SelectAllRequests) {
            c.BoxColor(0,0,w,h,0,PendingSurface);const bool checked=approvals.All();
            c.BoxColor(4,(h-16)/2,16,16,3,checked?Accent:D2D1::ColorF(0xFFFFFF),keyboardFocus?Accent:SoftBorder);
            if(checked)c.TextColor(L"✓",4,(h-20)/2,16,20,captionFormat,D2D1::ColorF(0xFFFFFF),DWRITE_TEXT_ALIGNMENT_CENTER);
            else if(approvals.Selected())c.BoxColor(8,(h-4)/2,8,4,1,Accent);
            c.TextColor(L"全选",28,0,w-28,h,captionFormat,TextInk);return;
        }
        if(d.CtlID==PendingRequests) {
            c.BoxColor(0,0,w,h,0,PendingSurface);
            if(d.itemID<approvals.rows.size()) {
                const auto &row=approvals.rows[d.itemID];const bool checked=approvals.Checked(row);
                c.BoxColor(1,1,w-2,h-2,4,row.sender?D2D1::ColorF(0xF0F3F9):D2D1::ColorF(0xFFFFFF),keyboardFocus?Accent:SoftBorder);
                if(row.sender) {
                    c.TextColor(approvals.collapsed.contains(row.peer)?L"›":L"⌄",7,0,22,h,strongFormat,TextMuted,DWRITE_TEXT_ALIGNMENT_CENTER);
                    c.Icon(1,34,(h-21)/2,.42f,.85f);c.TextColor(row.peer,64,0,w-224,h,strongFormat,TextInk);
                    c.TextColor(std::to_wstring(row.count)+L" 项",w-142,0,86,h,captionFormat,TextMuted,DWRITE_TEXT_ALIGNMENT_TRAILING);
                } else {
                    c.Icon(row.folder?3:2,36,(h-20)/2,.43f,.85f);c.TextColor(row.name,66,0,w-260,h,captionFormat,TextInk);
                    c.TextColor(FormatBytes(row.total),w-180,0,124,h,captionFormat,TextMuted,DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                c.BoxColor(w-35,(h-17)/2,17,17,3,checked?Accent:D2D1::ColorF(0xFFFFFF),SoftBorder);
                if(checked)c.TextColor(L"✓",w-35,(h-22)/2,17,22,captionFormat,D2D1::ColorF(0xFFFFFF),DWRITE_TEXT_ALIGNMENT_CENTER);
            }return;
        }
        if (d.CtlID == Devices) {
            c.BoxColor(0, 0, w, h, 0, SideSurface);
            if (shownPeers.empty()) {
                c.BoxColor(1, 3, w - 2, h - 6, 6, D2D1::ColorF(0xF8FAFC), SoftBorder);
                c.Icon(1, 15, (h - 18) / 2, .45f, .75f);
                c.TextColor(L"未发现在线设备", 47, 7, w - 59, 23, captionFormat, TextMuted);
                c.TextColor(L"可刷新或手动连接", 47, 30, w - 59, 23, captionFormat, TextMuted);
            } else if (d.itemID < shownPeers.size()) {
                auto &p = shownPeers[d.itemID];
                c.BoxColor(1, 3, w - 2, h - 6, 7, selected ? AccentLight : SideSurface,
                           keyboardFocus ? Accent : D2D1::ColorF(0, 0.f));
                if (selected) c.BoxColor(1, 12, 3, h - 24, 1.5f, Accent);
                c.Icon(p.browser ? 12 : 1, 18, 21, .29f, 1.3f);
                const float badgeWidth=p.platform=="windows"?96.f:p.platform=="android"?86.f:76.f,badgeX=w-27-badgeWidth;
                c.TextColor(p.name, 64, 11, std::max(22.f,badgeX-70), 27, strongFormat, TextInk);
                c.BoxColor(badgeX,14,badgeWidth,23,4,D2D1::ColorF(0xE6EEF9),D2D1::ColorF(0xCCDBEE));
                c.TextColor(PeerLabel(p),badgeX+3,14,badgeWidth-6,23,captionFormat,D2D1::ColorF(0x345474),DWRITE_TEXT_ALIGNMENT_CENTER);
                auto note = engine.GetNote(p.ip);
                c.TextColor(Wide(p.ip) + (note.empty() ? L"" : L" · " + note), 64, 37, w - 89, 23, captionFormat, TextMuted);
                c.BoxColor(w - 24, h / 2 - 5, 10, 10, 5, Success);
            }
            return;
        }
        if (d.CtlID == Tasks) {
            if (shownJobs.empty()) {
                c.BoxColor(1, 1, w - 2, h - 2, 7, D2D1::ColorF(0xFFFFFF), SoftBorder);
                c.Icon(2, w / 2 - 12, 20, .55f);
                c.TextColor(L"暂无传输任务", 12, 56, w - 24, 30, normal, TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (d.itemID < shownJobs.size()) {
                auto &j = shownJobs[d.itemID];
                const bool wide = width >= 1160;
                const bool active = j.state < State::Completed;
                const bool completed = j.state == State::Completed;
                if (j.childRow) {
                    const float indent=24.f+std::min(8u,j.depth)*24.f;
                    c.BoxColor(1,0,w-2,h,0,selected?D2D1::ColorF(0xEDF3FF):D2D1::ColorF(0xFFFFFF));
                    c.Line(20,h-1,w-20,h-1,.94f,1);
                    const bool open=j.directoryRow&&expandedFolders.contains(j.group+"|"+Utf8(j.treePath));
                    const float lineY=j.directoryRow?(h-28)/2:7.f;
                    if(j.directoryRow)c.Chevron(indent+4,h/2,open);
                    c.BoxColor(indent+19,lineY,28,28,6,j.directoryRow?D2D1::ColorF(0xFFF4D8):D2D1::ColorF(0xF1F4F8));
                    c.Icon(j.directoryRow?3:2,indent+23,lineY+4,j.directoryRow?.43f:.53f,.8f);
                    const float nameX=indent+58,statusX=w-178,sizeX=statusX-124;
                    c.TextColor(j.name,nameX,lineY,std::max(24.f,sizeX-nameX-20),28,captionFormat,TextInk);
                    c.TextColor(FormatBytes(j.total),sizeX,lineY,104,28,captionFormat,TextMuted,DWRITE_TEXT_ALIGNMENT_TRAILING);
                    const bool complete=j.state==State::Completed,failed=j.state==State::Failed;
                    const auto stateLabel=j.directoryRow?(complete?L"已完成  ":failed?L"有失败  ":L"")+std::to_wstring(j.completedFiles)+L" / "+std::to_wstring(j.folderCount):TransferAction(j);
                    c.BoxColor(statusX,lineY,158,28,14,complete?D2D1::ColorF(0xEAF7EF):failed?D2D1::ColorF(0xFFF0E9):D2D1::ColorF(0xF1F4F8));
                    c.TextColor(stateLabel,statusX+8,lineY,142,28,captionFormat,complete?Success:failed?D2D1::ColorF(0xB4523B):TextMuted,DWRITE_TEXT_ALIGNMENT_CENTER);
                    if(!j.directoryRow){
                        const double percent=j.total?100.0*j.done/j.total:j.state==State::Completed?100:0;
                        if(j.state==State::Running){const float track=std::min(160.f,std::max(40.f,w-nameX-275));c.BoxColor(nameX,42,track,5,2.5f,D2D1::ColorF(0xDEE3EC));if(percent>0)c.BoxColor(nameX,42,track*static_cast<float>(std::min(100.0,percent))/100,5,2.5f,Accent);c.TextColor((j.done&&percent<.1?L"<0.1":std::to_wstring(static_cast<unsigned>(percent)))+std::wstring(L"% · ")+FormatBytes(static_cast<uint64_t>(j.speed))+L"/s",nameX+track+12,31,w-nameX-track-26,25,captionFormat,TextMuted);}
                        else c.TextColor(j.error.empty()?FormatBytes(j.done)+L" / "+FormatBytes(j.total):j.error,nameX,31,w-nameX-14,25,captionFormat,j.state==State::Failed?D2D1::ColorF(0xB4523B):TextMuted);
                    }
                    return;
                }
                const float rowLeft = j.childRow ? 24.f : 1.f;
                c.BoxColor(rowLeft, 1, w - rowLeft - 1, h - 2, 4,
                           selected ? D2D1::ColorF(0xFAFCFF) : active?D2D1::ColorF(0xF4F8FF):D2D1::ColorF(0xF3F4F6), keyboardFocus ? Accent : SoftBorder);
                c.BoxColor(rowLeft+1,5,4,h-10,2,active?Accent:D2D1::ColorF(0xBCC4D0));
                if (j.childRow) c.Line(18, 0, 18, h, .86f, 1);
                const float actionX = w - 154, indent = j.childRow ? 24.f : 0.f;
                const float titleX = wide ? 210.f + indent : 46.f + indent;
                const float statusX = wide ? std::max(376.f, w * .47f) : titleX;
                const float statusWidth = std::max(36.f, actionX - statusX - 18);
                if (wide) {
                    c.BoxColor(14 + indent, 23, 34, 34, 17, j.receiving ? D2D1::ColorF(0xE3F7EA) : D2D1::ColorF(0xE4EDFF));
                    c.Icon(j.receiving ? 0 : 7, 19 + indent, 28, j.receiving ? .34f : .31f);
                    c.TextColor(j.receiving ? L"接收" : L"发送", 61 + indent, 13, 126, 25, strongFormat, TextInk);
                    c.TextColor((j.receiving ? L"来自 " : L"至 ") + j.peer, 61 + indent, 40, 104, 24, captionFormat, TextMuted);
                }
                c.Icon(j.folderRow ? 3 : 2, wide ? 172.f + indent : 14.f + indent, wide ? 24.f : 16.f, .36f, wide ? 1.25f : 1.f);
                const float titleWidth = wide ? statusX - titleX - 15 : actionX - titleX - 14;
                if(j.folderRow)c.Chevron(titleX+4,wide?25.f:20.f,expandedFolders.contains(j.group));
                c.TextColor(j.name,titleX+(j.folderRow?20.f:0.f),wide?12.f:7.f,titleWidth-(j.folderRow?20.f:0.f),27,strongFormat,TextInk);
                std::wstring metadata = j.folderRow ? std::to_wstring(j.completedFiles)+L"/"+std::to_wstring(j.folderCount)+L" 项 · "+FormatBytes(j.done)+L"/"+FormatBytes(j.total) : FormatBytes(j.total);
                metadata=(active?L"进行中 · ":L"已结束 · ")+metadata;
                if(j.folderRow&&j.skippedFiles)metadata+=L" · 跳过 "+std::to_wstring(j.skippedFiles)+L" 个文件";
                if (j.failedFiles) metadata += L" · "+std::to_wstring(j.failedFiles)+L" 个失败";
                if (j.cancelledFiles) metadata += L" · "+std::to_wstring(j.cancelledFiles)+L" 个未完成";
                if (j.folderRow && active && j.transferringFiles) metadata += L" · "+FormatBytes(static_cast<uint64_t>(j.speed))+L"/s";
                if (!wide) metadata = (j.receiving ? L"接收 · " : L"发送 · ") + j.peer + L" · " + metadata;
                c.TextColor(metadata, titleX, wide ? 40.f : 34.f, j.folderRow&&wide?actionX-titleX-12:titleWidth, 25, captionFormat, TextMuted);
                const auto statusLabel=j.error.empty()?TransferAction(j):j.error;
                const auto progressLabel=(j.state==State::Running&&!j.paused&&!j.folderRow?FormatBytes(static_cast<uint64_t>(j.speed))+L"/s · ":L"")+statusLabel;
                if (active) {
                    const float fraction = j.total ? static_cast<float>(std::min(1.0, static_cast<double>(j.done) / j.total)) : 0;
                    if (wide) {
                        const float trackWidth = std::max(8.f, statusWidth - 48);
                        c.BoxColor(statusX, 25, trackWidth, 10, 5, D2D1::ColorF(0xDEE3EC));
                        if (fraction > 0) c.BoxColor(statusX, 25, trackWidth * fraction, 10, 5, Accent);
                        c.TextColor(j.done && fraction*100<.1f ? L"<0.1%" : std::to_wstring(static_cast<unsigned>(fraction * 100)) + L"%", statusX + trackWidth + 6, 15, 48, 28, captionFormat, TextInk);
                        if(!j.folderRow)c.TextColor(progressLabel,
                                    statusX, 42, statusWidth, 22, captionFormat, TextMuted);
                    } else {
                        const float trackWidth = std::max(8.f, actionX - titleX - 10);
                        c.BoxColor(titleX, h - 12, trackWidth, 5, 2.5f, D2D1::ColorF(0xDEE3EC));
                        if (fraction > 0) c.BoxColor(titleX, h - 12, trackWidth * fraction, 5, 2.5f, Accent);
                    }
                } else if (wide) {
                    const auto label=j.folderRow?(j.state==State::Failed?L"部分失败":TransferAction(j)):j.error.empty()?completed?(j.receiving?L"接收完成":L"发送完成"):TransferAction(j):j.error;
                    c.TextColor(label,statusX,j.folderRow?11.f:22.f,statusWidth,j.folderRow?27.f:34.f,normal,completed?Success:TextMuted);
                }
                if (!wide) c.TextColor(j.folderRow?(j.error.empty()?StateText(j.state,j.receiving):j.error):progressLabel, titleX, 59,
                                      titleWidth, 22, captionFormat, completed ? Success : TextMuted);
                if (j.folderRow && active) {
                    const auto detail = j.currentAction + (j.currentFile.empty() ? L"" : L"：" + j.currentFile)+
                        (j.currentTransferring?L"  ·  "+FormatBytes(j.currentDone)+L" / "+FormatBytes(j.currentTotal):L"")+
                        (j.stageSummary.empty()?L"":L"  ·  "+j.stageSummary);
                    IDWriteInlineObject *ellipsis = nullptr;
                    write->CreateEllipsisTrimmingSign(captionFormat,&ellipsis);
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,L'\\',1};
                    captionFormat->SetTrimming(&trimming,ellipsis);
                    c.TextColor(detail,titleX,wide?72.f:84.f,w-titleX-24,22,captionFormat,Accent);
                    const DWRITE_TRIMMING none{};captionFormat->SetTrimming(&none,nullptr);
                    if(ellipsis)ellipsis->Release();
                }
                c.BoxColor(actionX, (h - 38) / 2, 92, 38, 5, D2D1::ColorF(0xF9FAFC), SoftBorder);
                const bool retry = j.canRetry && !j.receiving && j.state >= State::Cancelled;
                c.TextColor(active?(j.paused?L"继续":L"暂停"):retry ? L"重试" : L"打开文件夹", actionX + 3, (h - 38) / 2, 86, 38, captionFormat,
                            !active&&!retry && j.savedPath.empty() ? D2D1::ColorF(0xA3AAB5) : Accent, DWRITE_TEXT_ALIGNMENT_CENTER);
                c.BoxColor(w - 58, (h - 38) / 2, 50, 38, 5, D2D1::ColorF(0xF9FAFC), SoftBorder);
                c.TextColor(active ? L"取消" : L"删除", w - 56, (h - 38) / 2, 46, 38, captionFormat, Accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            return;
        }
        if (d.CtlID == ToggleAuto) {
            c.TextColor(L"自动接收", 0, 0, 86, h, normal, TextInk);
            c.BoxColor(98, h / 2 - 11, 44, 22, 11, snapshot.config.autoReceive ? Accent : D2D1::ColorF(0xA6A9AF));
            c.Circle(snapshot.config.autoReceive ? 131.f : 109.f, h / 2, 8.f, 1.f, true);
            return;
        }
        if (d.CtlID == AllTasks || d.CtlID == SentTasks || d.CtlID == ReceivedTasks) {
            const bool active = taskFilter == static_cast<int>(d.CtlID) - AllTasks;
            c.BoxColor(1, 1, w - 2, h - 2, 5, active ? Accent : D2D1::ColorF(0xF1F4F8));
            wchar_t label[64]{}; GetWindowTextW(d.hwndItem, label, 64);
            c.TextColor(label, 0, 0, w, h, captionFormat, active ? D2D1::ColorF(0xFFFFFF) : TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (d.CtlID == Drop) {
            const bool compact = h < 200, shortWindow = h < 180;
            c.DashedBoxColor(1, 1, w - 2, h - 2, 7,
                             disabled ? D2D1::ColorF(0xFAFBFD) : hover ? D2D1::ColorF(0xF6F9FF) : D2D1::ColorF(0xFDFEFF),
                             keyboardFocus ? Accent : D2D1::ColorF(0xC5D0E1));
            if (!shortWindow) c.Icon(7, w / 2 - (compact ? 17.f : 22.f), compact ? 14.f : 23.f, disabled ? .65f : .34f, compact ? 1.4f : 1.8f);
            c.TextColor(L"拖入文件或文件夹", 20, h < 120 ? 0.f : shortWindow ? 4.f : compact ? 51.f : 82.f, w - 40, h < 120 ? 28.f : 36.f,
                        compact ? section : heroFormat, disabled ? TextMuted : TextInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (h >= 120) c.TextColor(displayedPeer, 20, shortWindow ? 40.f : compact ? 86.f : 120.f, w - 40, 27, normal, TextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (d.CtlID == OpenFolder && GetParent(d.hwndItem) == window) {
            c.BoxColor(0, 0, w, h, 0, D2D1::ColorF(0xF9FAFC));
            c.BoxColor(1, 1, w - 2, h - 2, 5, selected ? AccentLight : hover ? D2D1::ColorF(0xEAF0FA) : D2D1::ColorF(0xF9FAFC),
                       keyboardFocus ? Accent : D2D1::ColorF(0, 0.f));
            c.Icon(3, 8, (h - 24) / 2, .35f);
            c.TextColor(L"接收目录：" + snapshot.config.receiveDir.wstring(), 38, 0, w - 132, h, captionFormat, TextMuted);
            c.TextColor(L"查看 / 修改", w - 88, 0, 82, h, captionFormat, Accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        wchar_t text[100]{}; GetWindowTextW(d.hwndItem, text, 100);
        const bool modal = GetParent(d.hwndItem) != window;
        const bool side = d.CtlID == Refresh || d.CtlID == PingPeer || d.CtlID == NotePeer || d.CtlID == ConnectPeer;
        if (side && !modal) c.BoxColor(0, 0, w, h, 0, SideSurface);
        const bool solid = d.CtlID == Accept || d.CtlID == SelectFiles || (modal && d.CtlID == 9);
        const bool quiet = d.CtlID == Refresh || d.CtlID == ConnectPeer || d.CtlID == Clear;
        D2D1_COLOR_F fill = disabled && solid ? D2D1::ColorF(0xE5EBF6) : solid ? (selected ? D2D1::ColorF(0x144CC8) : hover ? D2D1::ColorF(0x1556E0) : Accent)
                           : hover ? D2D1::ColorF(0xEAF0FA) : quiet ? side ? SideSurface : D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0xF9FAFC);
        c.BoxColor(1, 1, w - 2, h - 2, 5, fill, keyboardFocus ? Accent : quiet ? D2D1::ColorF(0, 0.f) : d.CtlID == SelectFolder ? Accent : SoftBorder);
        const auto color = disabled ? D2D1::ColorF(0x9AA3B3) : solid ? D2D1::ColorF(0xFFFFFF) : d.CtlID == SelectFolder ? Accent : TextInk;
        const float iconGray = disabled ? .64f : solid ? 1.f : .31f;
        if (d.CtlID==SelectFiles || d.CtlID==SelectFolder) {
            c.Icon(d.CtlID==SelectFiles?2:3,11,(h-22)/2,iconGray,.9f);
            c.TextColor(text,35,0,w-40,h,normal,color,DWRITE_TEXT_ALIGNMENT_CENTER);return;
        }
        const int glyph = d.CtlID == WebService ? 8 : d.CtlID == PingPeer ? 9 : d.CtlID == NotePeer ? 10 : d.CtlID == Settings ? 4
                        : d.CtlID == Refresh ? 6 : d.CtlID == ConnectPeer ? 5 : d.CtlID == Clear ? 11 : -1;
        if (glyph >= 0) {
            const bool navigation = d.CtlID == ConnectPeer;
            const float x = d.CtlID == Refresh ? (w - 24) / 2 : navigation ? 8.f : 11.f;
            c.Icon(glyph, x, (h - 24) / 2, iconGray);
            const float labelX = navigation ? 46.f : d.CtlID == NotePeer ? 38.f : 42.f;
            if (d.CtlID != Refresh) c.TextColor(text, labelX, 0, w - labelX - 4, h, normal, color);
        } else c.TextColor(text, 0, 0, w, h, normal, color, DWRITE_TEXT_ALIGNMENT_CENTER);
        const auto updateState = updater.Get();
        if ((d.hwndItem == settingsButton && updateState.settingsUnread) ||
            (modal && d.CtlID == 17 && updateState.updateUnread))
            c.BoxColor(w - 11, 4, 7, 7, 3.5f, D2D1::ColorF(0xE5484D));
    }

    void SendPaths(const std::vector<fs::path> &files) {
        auto it = std::find_if(shownPeers.begin(), shownPeers.end(), [&](auto &p) { return p.id == peerId; });
        if (it == shownPeers.end()) {
            localNotice = L"未选择在线目标设备";
            Update();
            return;
        }
        bool pack=false;
        if(it->browser&&!it->directoryReceive&&!snapshot.config.zipFolders&&std::any_of(files.begin(),files.end(),[](const auto &path){return fs::is_directory(path);})){
            const TASKDIALOG_BUTTON choices[]={{IDYES,L"打包传输"},{IDCANCEL,L"取消"}};TASKDIALOGCONFIG dialog{};dialog.cbSize=sizeof(dialog);dialog.hwndParent=window;dialog.pszWindowTitle=L"接收端无法接收文件夹";dialog.pszMainInstruction=L"目标 Web 页面不支持直接保存目录";dialog.pszContent=L"建议将文件夹打包为 ZIP 后发送。此操作仅对本次传输生效。";dialog.cButtons=2;dialog.pButtons=choices;dialog.nDefaultButton=IDYES;int selected=IDCANCEL;TaskDialogIndirect(&dialog,&selected,nullptr,nullptr);if(selected!=IDYES)return;pack=true;
        }
        engine.SendFiles(*it, files,pack);
        localNotice = L"";
        Update();
    }
    void ChooseFiles(UINT selection = 0) {
        if (!dropEnabled)
            return;
        if (!selection) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"选择文件…");
        AppendMenuW(menu, MF_STRING, 2, L"选择文件夹…");
        RECT bounds{}; GetWindowRect(drop, &bounds);
        selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_CENTERALIGN,
            (bounds.left + bounds.right) / 2, bounds.top + Px(140), 0, window, nullptr);
        DestroyMenu(menu);
        }
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
    void QrDialog();
    void ReceiveDirectoryDialog();
    void NoteDialog();
    void PartialDialog();
    int TaskHeight(size_t index) const {
        return Px(index<shownJobs.size() ? shownJobs[index].childRow ? shownJobs[index].directoryRow ? 48.f : 66.f : static_cast<float>(taskRowHeight+(shownJobs[index].folderRow?24:0)) : height<720?100.f:124.f);
    }
    void TaskHeights(){for(size_t i=0;i<std::max<size_t>(1,shownJobs.size());++i)SendMessageW(tasks,LB_SETITEMHEIGHT,i,TaskHeight(i));}
    bool ToggleTask(WPARAM key = VK_SPACE) {
        const auto found=std::find_if(shownJobs.begin(),shownJobs.end(),[&](const auto &row){return row.id==selectedTask;});
        if (found==shownJobs.end() || (!found->folderRow&&!found->directoryRow)) return false;
        const auto keyValue=found->group+(found->directoryRow?"|"+Utf8(found->treePath):"");
        const bool open=expandedFolders.contains(keyValue);
        if(key==VK_LEFT&&!open||key==VK_RIGHT&&open)return true;
        if(open)expandedFolders.erase(keyValue);else expandedFolders.insert(keyValue);
        Update();return true;
    }
    void CancelTask() {
        auto found = std::find_if(shownJobs.begin(), shownJobs.end(), [&](auto &j) { return j.id == selectedTask; });
        if (found == shownJobs.end()) return;
        if (found->folderRow && !found->members.empty()) engine.Cancel(found->members.front());
        else engine.Cancel(selectedTask);
        Update();
    }
    void OpenOrRetryTask() {
        auto found = std::find_if(shownJobs.begin(), shownJobs.end(), [&](auto &j) { return j.id == selectedTask; });
        if(found!=shownJobs.end()&&found->state<State::Completed){engine.Pause(found->folderRow&&!found->members.empty()?found->members.front():found->id,!found->paused);Update();return;}
        if (found != shownJobs.end() && found->canRetry && !found->receiving && found->state >= State::Cancelled) {
            std::wstring error;
            if(found->folderRow&&found->state==State::Failed){for(const auto &job:snapshot.transfers)if(job.state==State::Failed&&std::find(found->members.begin(),found->members.end(),job.id)!=found->members.end())engine.RetryTransfer(job.id,error);localNotice=error.empty()?L"失败文件已加入独立重试通道":error;Update();return;}
            auto id = found->id;
            if (found->folderRow) for (const auto &job : snapshot.transfers)
                if (job.state >= State::Cancelled && std::find(found->members.begin(), found->members.end(), job.id) != found->members.end()) { id = job.id; break; }
            localNotice = engine.RetryTransfer(id, error) ? L"已重新发起发送，等待接收方确认" : error;
            Update(); return;
        }
        OpenTaskFile();
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
        auto message = std::wstring(found->folderRow ? L"删除此批次记录及本次传输的文件。目录和跳过的已有文件将保留。\n\n" : L"删除此传输记录及对应文件。目录和跳过的已有文件将保留。\n\n") +
                       (found->savedPath.empty() ? found->name : found->savedPath.wstring());
        if (MessageBoxW(window, message.c_str(), L"删除记录和文件",
                        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
        std::wstring error;
        bool removed = true;
        if (found->folderRow) { for (auto id : found->members) if (!engine.DeleteTransfer(id, true, error)) removed = false; }
        else removed = engine.DeleteTransfer(selectedTask, true, error);
        localNotice = removed ? L"记录已删除；目录和跳过的已有文件已保留" : error;
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
            ChooseFiles(1);
            break;
        case SelectFiles:
            ChooseFiles(1);
            break;
        case SelectFolder:
            ChooseFiles(2);
            break;
        case ToggleAuto: {
            auto config = snapshot.config; config.autoReceive = !config.autoReceive;
            std::wstring error;
            if (!engine.UpdateConfig(config, error)) localNotice = error;
            else localNotice = config.autoReceive ? L"自动接收已开启" : L"自动接收已关闭";
            Update();
            break;
        }
        case AllTasks: case SentTasks: case ReceivedTasks:
            taskFilter = id - AllTasks;
            InvalidateRect(allButton, nullptr, FALSE); InvalidateRect(sentButton, nullptr, FALSE); InvalidateRect(receivedButton, nullptr, FALSE);
            Update();
            break;
        case ShowQr:
            QrDialog();
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
        case CopyUrl:
            if (snapshot.config.shareSoftware) CopyAddress(webAddress);
            break;
        case OpenUrl:
            if (snapshot.config.shareSoftware) {
                wchar_t address[256]{}; GetWindowTextW(webAddress, address, 256);
                ShellExecuteW(window, L"open", address, nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case OpenFolder:
            ReceiveDirectoryDialog();
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
        case SelectAllRequests:
            approvals.ToggleAll();RefreshPending();
            break;
        case Accept:
            for(const auto &[requestId,mask]:approvals.Decisions(pendingJobs,true))engine.DecideItems(requestId,mask);
            Update();
            break;
        case Reject:
            for(const auto &[requestId,mask]:approvals.Decisions(pendingJobs,false))engine.DecideItems(requestId,mask);
            Update();
            break;
        default:
            break;
        }
    }
};

struct Modal {
    App &app;
    HWND window = nullptr, name = nullptr, path = nullptr, autoReceive = nullptr, share = nullptr, zipFolders = nullptr,
         port = nullptr, help = nullptr, maxFiles = nullptr, maxBytes = nullptr, itemList = nullptr;
    bool qr = false;std::unique_ptr<qrcodegen::QrCode> qrCode;
    bool settings = false, done = false, web = false, note = false, approval = false, directory = false;
    bool updating = false, openUpdater = false;
    HWND updateAction = nullptr, updateProgress = nullptr, updateNotes = nullptr, updateCancel = nullptr;
    bool updateEntryDot = false;
    void UpdateStatus(const update::Status *sample = nullptr) {
        if (settings || updating) app.updater.SettingsRead();
        if (updating) app.updater.UpdateRead();
        const auto state = sample ? *sample : app.updater.Get();
        if (settings && state.updateUnread != updateEntryDot) { updateEntryDot = state.updateUnread; InvalidateRect(GetDlgItem(window, 17), nullptr, FALSE); }
        if (!updating) return;
        const bool busy = state.phase == update::Phase::Checking || state.phase == update::Phase::Downloading;
        auto message = state.message;
        if (state.phase == update::Phase::Downloading && state.total)
            message += L"  " + FormatBytes(state.done) + L" / " + FormatBytes(state.total);
        SetWindowTextW(help, message.c_str());
        EnableWindow(updateAction, !busy);
        SetWindowTextW(updateAction, state.phase == update::Phase::Ready ? L"立即重启" : state.phase == update::Phase::Available ? L"下载更新" : L"检查更新");
        EnableWindow(updateCancel, busy);
        ShowWindow(updateProgress, state.phase == update::Phase::Downloading ? SW_SHOW : SW_HIDE);
        ShowWindow(updateCancel, busy ? SW_SHOW : SW_HIDE);
        SetWindowTextW(updateCancel, state.phase == update::Phase::Checking ? L"取消检查" : L"取消下载");
        ShowWindow(updateNotes, state.notes.empty() ? SW_HIDE : SW_SHOW);
        SendMessageW(updateProgress, PBM_SETPOS, state.total ? static_cast<WPARAM>(state.done * 100 / state.total) : 0, 0);
        if (Value(updateNotes) != state.notes) SetWindowTextW(updateNotes, state.notes.c_str());
    }
    void UpdateAction() {
        const auto state = app.updater.Get();
        if (state.phase == update::Phase::Ready) {
            if (app.engine.HasActiveTransfers()) { MessageBoxW(window, L"当前存在未结束的传输，请结束任务后重启更新。", L"在线更新", MB_OK | MB_ICONINFORMATION); return; }
            std::wstring error;
            if (!app.updater.Restart(window, app.engine.GetSnapshot().config.dataDir, error)) { MessageBoxW(window, error.c_str(), L"在线更新", MB_OK | MB_ICONERROR); return; }
            done = true;
            PostMessageW(app.window, WM_CLOSE, 0, 0);
        } else if (state.phase == update::Phase::Available) app.updater.Download();
        else app.updater.Check();
        UpdateStatus();
    }
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
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(wcscmp(cls, L"STATIC") == 0 ? app.smallFont : app.font), TRUE);
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
            if(qr || updating)return;
            for (auto &r : editFrames)
                c.Box(r.left, r.top, r.right - r.left, r.bottom - r.top, 7, 1, .84f);
            c.Line(24,
                   web || directory ? 184.f
                   : settings ? 444.f : approval ? 420.f
                              : 152.f,
                   w - 24,
                   web || directory ? 184.f
                   : settings ? 444.f : approval ? 420.f
                              : 152.f,
                   .9f, 1);
        });
        if(qr&&qrCode){
            const int count=qrCode->getSize(),step=std::max(1,app.Px(272.f)/(count+8));
            const int left=(r.right-step*(count+8))/2,top=app.Px(100.f);
            RECT background{left,top,left+step*(count+8),top+step*(count+8)};FillRect(ps.hdc,&background,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            for(int y=0;y<count;++y)for(int x=0;x<count;++x)if(qrCode->getModule(x,y)){
                RECT pixel{left+(x+4)*step,top+(y+4)*step,left+(x+5)*step,top+(y+5)*step};FillRect(ps.hdc,&pixel,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
        }
        EndPaint(window, &ps);
    }
    void UpdateQr(){try{const auto url=Utf8(Value(name));qrCode=std::make_unique<qrcodegen::QrCode>(qrcodegen::QrCode::encodeText(url.c_str(),qrcodegen::QrCode::Ecc::MEDIUM));InvalidateRect(window,nullptr,FALSE);}catch(...){qrCode.reset();}}
    std::wstring Value(HWND c) {
        int n = GetWindowTextLengthW(c);
        std::wstring text(static_cast<size_t>(n) + 1, 0);
        GetWindowTextW(c, text.data(), n + 1);
        text.resize(n);
        return text;
    }
    void Create() {
        auto &cfg = app.snapshot.config;
        if (updating) {
            app.updater.UpdateRead();
            Control(L"STATIC", (L"当前版本 V" + std::wstring(Version) + L" · Windows x64").c_str(), 0, 24, 20, 512, 24, 0);
            help = Control(L"STATIC", L"", SS_LEFT, 24, 54, 512, 54, 0);
            updateProgress = Control(PROGRESS_CLASSW, L"", 0, 24, 112, 512, 16, 0);
            updateNotes = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 24, 148, 512, 164, 0);
            Control(L"BUTTON", L"发布页", WS_TABSTOP, 24, 350, 88, 34, 19);
            updateCancel = Control(L"BUTTON", L"取消下载", WS_TABSTOP, 124, 350, 104, 34, 18);
            Control(L"BUTTON", L"关闭", WS_TABSTOP, 328, 350, 88, 34, 8);
            updateAction = Control(L"BUTTON", L"检查更新", WS_TABSTOP, 428, 350, 108, 34, 17);
            SetTimer(window, 2, 200, nullptr); UpdateStatus();
        } else if(qr){
            Control(L"STATIC",L"扫描二维码访问 Web 服务",0,24,18,352,24,0);
            name=Control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_HASSTRINGS|WS_VSCROLL,24,52,352,140,1);
            auto addresses=app.snapshot.interfaces;
            std::stable_sort(addresses.begin(),addresses.end(),[](const auto &a,const auto &b){const bool ap=!a.virtualAdapter&&!a.ip.starts_with("169.254."),bp=!b.virtualAdapter&&!b.ip.starts_with("169.254.");return ap>bp;});
            for(const auto &address:addresses)if(!address.ip.starts_with("127.")&&address.ip!="0.0.0.0"){
                const auto url=L"http://"+Wide(address.ip)+L":"+std::to_wstring(app.snapshot.boundPort)+L"/";SendMessageW(name,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(url.c_str()));
            }
            SendMessageW(name,CB_SETCURSEL,0,0);
            help=Control(L"STATIC",L"设备需处于同一局域网。",SS_LEFT,24,378,352,24,0);
            Control(L"BUTTON",L"关闭",WS_TABSTOP|BS_PUSHBUTTON,292,414,84,33,8);UpdateQr();
        } else if (approval) {
            Control(L"STATIC", L"选择要接收的项目", 0, 24, 20, 560, 28, 0);
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
        } else if (directory) {
            Control(L"STATIC", L"接收目录", 0, 24, 20, 542, 24, 0);
            path = Control(L"EDIT", cfg.receiveDir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 24, 53, 438, 34, 1);
            Control(L"BUTTON", L"选择目录", WS_TABSTOP, 474, 53, 92, 34, 3);
            help = Control(L"STATIC", L"可查看目录中的文件，或修改路径后保存。", SS_LEFT, 24, 106, 542, 58, 0);
            Control(L"BUTTON", L"查看目录", WS_TABSTOP, 24, 200, 106, 34, 12);
            Control(L"BUTTON", L"取消", WS_TABSTOP, 378, 200, 88, 34, 8);
            Control(L"BUTTON", L"保存", WS_TABSTOP, 478, 200, 88, 34, 9);
            name = path;
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
            Control(L"STATIC", L"提供局域网文件传输、Web 服务与 Windows、Android 客户端下载。", SS_LEFT, 24, 103, 542, 36, 0);
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
            zipFolders = Control(L"BUTTON", L"发送文件夹时打包为 ZIP（默认关闭）", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 392, 420, 28, 14);
            SendMessageW(zipFolders, BM_SETCHECK, cfg.zipFolders ? BST_CHECKED : BST_UNCHECKED, 0);
            help = Control(L"STATIC", L"关闭时逐文件复制，跳过内容相同的文件。", SS_LEFT, 24, 424, 420, 22, 0);
            Control(L"BUTTON", L"导出新增日志", WS_TABSTOP | BS_PUSHBUTTON, 24, 457, 140, 33, 15);
            Control(L"BUTTON", L"清除日志", WS_TABSTOP | BS_PUSHBUTTON, 174, 457, 84, 33, 16);
            Control(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, 268, 457, 81, 33, 8);
            Control(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, 360, 457, 84, 33, 9);
            Control(L"STATIC", (L"当前版本 V" + std::wstring(Version)).c_str(), 0, 24, 510, 260, 24, 0);
            Control(L"BUTTON", L"检查更新", WS_TABSTOP, 336, 505, 108, 33, 17);
            app.updater.SettingsRead();
            InvalidateRect(app.settingsButton, nullptr, FALSE);
            SetTimer(window, 2, 200, nullptr); UpdateStatus();
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
        picker->SetTitle(L"选择接收目录");
        IShellItem *current = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(Value(path).c_str(), nullptr, IID_PPV_ARGS(&current)))) {
            picker->SetFolder(current);
            current->Release();
        }
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
    void OpenDirectory() {
        auto selected = fs::path(Value(path));
        std::error_code error;
        if (!selected.is_absolute() || !fs::is_directory(selected, error)) {
            SetWindowTextW(help, L"目录不存在或不可访问，请选择已有目录，或先保存新路径。");
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", selected.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            SetWindowTextW(help, L"无法打开目录，请检查目录是否可访问。");
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
        if (directory) {
            Config cfg = app.engine.GetSnapshot().config;
            cfg.receiveDir = Value(path);
            if (!app.engine.UpdateConfig(cfg, error)) {
                SetWindowTextW(help, error.c_str());
                return;
            }
            app.localNotice = L"接收目录已保存";
            done = true;
        } else if (settings) {
            Config cfg = app.snapshot.config;
            cfg.name = Value(name);
            cfg.receiveDir = Value(path);
            cfg.zipFolders = SendMessageW(zipFolders, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
            if (HIWORD(w) == BN_CLICKED && LOWORD(w) == 17) {
                if (self->updating) self->UpdateAction();
                else if (self->settings) { self->openUpdater = true; self->done = true; }
                return 0;
            }
            if (self->updating && LOWORD(w) == 18) { self->app.updater.Cancel(); return 0; }
            if (self->updating && LOWORD(w) == 19) { ShellExecuteW(h, L"open", update::ReleasePage, nullptr, nullptr, SW_SHOWNORMAL); return 0; }
            if(self->qr&&LOWORD(w)==1&&HIWORD(w)==CBN_SELCHANGE)self->UpdateQr();
            if(self->settings&&LOWORD(w)==16&&HIWORD(w)==BN_CLICKED){const bool cleared=self->app.engine.ClearDiagnostics();MessageBoxW(h,cleared?L"服务端日志已清除。":L"日志清除失败，请检查文件权限。",L"诊断日志",MB_OK|(cleared?MB_ICONINFORMATION:MB_ICONERROR));}
            if(self->settings&&LOWORD(w)==15&&HIWORD(w)==BN_CLICKED){
                try{const auto file=self->app.engine.ExportDiagnostics();ShellExecuteW(h,L"open",L"explorer.exe",(L"/select,\""+file.wstring()+L"\"").c_str(),nullptr,SW_SHOWNORMAL);}
                catch(const std::exception &error){MessageBoxW(h,Wide(error.what()).c_str(),L"日志导出失败",MB_OK|MB_ICONERROR);}
            }
            if (self->directory && LOWORD(w) == 12 && HIWORD(w) == BN_CLICKED)
                self->OpenDirectory();
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
        case WM_TIMER:
            self->UpdateStatus();
            return 0;
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
        int h = updating ? 410 : qr ? 468 : web || directory ? 254 : settings ? 562 : approval ? 496 : 232;
        RECT r{0, 0, app.Px(updating ? 560.f : qr ? 400.f : approval ? 608.f : web || directory ? 590.f : 470.f), app.Px(static_cast<float>(h))};
        AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_DLGMODALFRAME,
                                 GetDpiForWindow(app.window));
        RECT owner{};
        GetWindowRect(app.window, &owner);
        window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ChuanModal",
                                 updating ? L"在线更新" : qr ? L"Web 服务二维码" : directory  ? L"接收目录"
                                 : web      ? L"Web 服务"
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
                const auto focusedId = GetDlgCtrlID(GetFocus());
                wchar_t focusedClass[32]{}; GetClassNameW(GetFocus(), focusedClass, 32);
                if (wcscmp(focusedClass, L"Button") == 0)
                    SendMessageW(window, WM_COMMAND, MAKEWPARAM(focusedId, BN_CLICKED), reinterpret_cast<LPARAM>(GetFocus()));
                else if (updating) UpdateAction();
                else Save();
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
        if (openUpdater) { Modal dialog(app, false); dialog.updating = true; dialog.Run(); }
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
void App::QrDialog(){
    if(!snapshot.config.shareSoftware)return;
    const bool available=std::any_of(snapshot.interfaces.begin(),snapshot.interfaces.end(),[](const auto &item){return !item.ip.starts_with("127.")&&item.ip!="0.0.0.0";});
    if(!available){MessageBoxW(window,L"未检测到可供局域网访问的 IPv4 地址。",L"Web 服务二维码",MB_OK|MB_ICONINFORMATION);return;}
    Modal dialog(*this,false);dialog.qr=true;dialog.Run();
}
void App::WebDialog() {
    Modal(*this, false, true).Run();
}
void App::ReceiveDirectoryDialog() {
    Modal dialog(*this, false); dialog.directory = true; dialog.Run();
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
        const auto bounds = DefaultWindowBounds(WindowWorkArea(h));
        m->ptMinTrackSize = {std::min<LONG>(app->Px(920), bounds.right - bounds.left),
                             std::min<LONG>(app->Px(660), bounds.bottom - bounds.top)};
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
        { auto *measure=reinterpret_cast<MEASUREITEMSTRUCT *>(l);measure->itemHeight=measure->CtlID==Tasks?app->TaskHeight(measure->itemID):app->Px(80); }
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        if (reinterpret_cast<HWND>(l) == app->flash) { SetBkColor(reinterpret_cast<HDC>(w), RGB(210,210,210)); return reinterpret_cast<LRESULT>(app->pingBrush); }
        if (reinterpret_cast<HWND>(l) == app->request) { SetTextColor(reinterpret_cast<HDC>(w), Muted); SetBkColor(reinterpret_cast<HDC>(w), RGB(255,248,233)); return reinterpret_cast<LRESULT>(app->pendingBrush); }
        if (reinterpret_cast<HWND>(l) == app->webAddress) { SetTextColor(reinterpret_cast<HDC>(w), Muted); SetBkColor(reinterpret_cast<HDC>(w), RGB(249,250,252)); return reinterpret_cast<LRESULT>(app->footerBrush); }
        SetTextColor(reinterpret_cast<HDC>(w), Muted);
        SetBkColor(reinterpret_cast<HDC>(w),
                   reinterpret_cast<HWND>(l) == app->status ? RGB(249, 249, 249) : RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(reinterpret_cast<HWND>(l) == app->status ? app->surface
                                                                                  : app->white);
    }
    case WM_CTLCOLORLISTBOX: {
        if (reinterpret_cast<HWND>(l) == app->requestList) return reinterpret_cast<LRESULT>(app->pendingBrush);
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
    if (const auto helper = update::RunHelper(options); helper >= 0) { CoUninitialize(); return helper; }
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
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS};
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
    const auto r = DefaultWindowBounds(WindowWorkArea());
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"传 · 局域网文件传输",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, r.left, r.top,
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
    app.updater.Check();
    if (options.contains(L"--update-event")) {
        const auto &name = options[L"--update-event"];
        if (name.starts_with(L"Local\\ChuanUpdate-")) {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
            if (ready) { SetEvent(ready); CloseHandle(ready); }
        }
    }
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
