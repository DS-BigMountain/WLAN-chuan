#define CHUAN_UI_TEST_BUILD
#include "../src/app.cpp"
#include <stdexcept>

static void SaveBitmap(HDC dc, HBITMAP bitmap, int width, int height, const fs::path &path) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4);
    if (!GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)) throw std::runtime_error("capture bitmap");
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char *>(&header),sizeof(header));
    file.write(reinterpret_cast<const char *>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(reinterpret_cast<const char *>(pixels.data()),pixels.size());
}
static void Capture(App &app,const fs::path &path) {
    const int width=app.Px(static_cast<float>(app.width)),height=app.Px(static_cast<float>(app.height));
    HDC screen=GetDC(app.window),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    auto old=SelectObject(dc,bitmap);RECT full{0,0,width,height};
    app.Buffered(dc,full,1,[&](ui::Canvas &canvas,float,float){app.DrawBackground(canvas);});
    auto control=[&](HWND child,int id,int item,float background){
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT *>(&bounds),2);
        if(item>=0){const int itemHeight=static_cast<int>(SendMessageW(child,LB_GETITEMHEIGHT,0,0));bounds.top+=item*itemHeight;bounds.bottom=std::min(bounds.bottom,bounds.top+itemHeight);}
        if(bounds.bottom<=bounds.top)return;
        DRAWITEMSTRUCT draw{};draw.CtlID=id;draw.hwndItem=child;draw.itemID=item<0?0:static_cast<UINT>(item);
        if(!IsWindowEnabled(child))draw.itemState|=ODS_DISABLED;
        if(id==Devices&&draw.itemID<app.shownPeers.size()&&app.shownPeers[draw.itemID].id==app.peerId)draw.itemState|=ODS_SELECTED;
        app.Buffered(dc,bounds,background,[&](ui::Canvas &canvas,float w,float h){app.DrawControl(canvas,draw,w,h,background);});
    };
    if(app.shownPeers.empty())control(app.devices,Devices,0,.963f);
    if(app.shownJobs.empty())control(app.tasks,Tasks,0,1.f);
    for(size_t i=0;i<app.shownPeers.size();++i)control(app.devices,Devices,static_cast<int>(i),.963f);
    for(size_t i=0;i<app.shownJobs.size();++i)control(app.tasks,Tasks,static_cast<int>(i),1.f);
    for(auto [child,id]:{std::pair{app.drop,Drop},{app.filesButton,SelectFiles},{app.sendFolderButton,SelectFolder},
        {app.autoButton,ToggleAuto},{app.settingsButton,Settings},{app.refreshButton,Refresh},{app.pingButton,PingPeer},
        {app.noteButton,NotePeer},{app.connectButton,ConnectPeer},{app.folderButton,OpenFolder},{app.webButton,WebService},
        {app.allButton,AllTasks},{app.sentButton,SentTasks},{app.receivedButton,ReceivedTasks},{app.clearButton,Clear},
        {app.webCopyButton,CopyUrl},{app.webOpenButton,OpenUrl}})
        if(IsWindowVisible(child)||(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE))control(child,id,-1,id==PingPeer||id==NotePeer||id==ConnectPeer||id==Refresh?.963f:1.f);
    if(app.pending){control(app.reject,Reject,-1,1);control(app.partial,Partial,-1,1);control(app.accept,Accept,-1,1);}
    for(HWND child:{app.request,app.status,app.webAddress}) {
        if(child==app.request&&!app.pending)continue;
        if(child==app.webAddress&&app.width<1160)continue;
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT *>(&bounds),2);
        wchar_t text[512]{};GetWindowTextW(child,text,512);
        app.Buffered(dc,bounds,1,[&](ui::Canvas &canvas,float w,float h){canvas.BoxColor(0,0,w,h,0,child==app.request?PendingSurface:child==app.webAddress?D2D1::ColorF(0xF9FAFC):D2D1::ColorF(0xF9F9F9));canvas.TextColor(text,0,0,w,h,app.captionFormat,TextMuted);});
    }
    SelectObject(dc,old);SaveBitmap(dc,bitmap,width,height,path);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(app.window,screen);
}
int wmain(int argc,wchar_t **argv) {
    if(argc!=2)return 2;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES};InitCommonControlsEx(&controls);
    const auto root=fs::absolute(argv[1]);fs::create_directories(root);
    auto config=LoadConfig(root/L"config");config.name=L"BIGMOUNTAIN";config.automaticName=false;config.autoReceive=false;config.receiveDir=root/L"received";
    Engine engine(config);App app(engine);WNDCLASSW cls{};cls.lpfnWndProc=WindowProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ChuanLayoutCapture";RegisterClassW(&cls);
    RECT bounds{0,0,1280,800};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"传 · 1.1",WS_OVERLAPPEDWINDOW,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,&app);
    KillTimer(window,1);app.snapshot.config=config;app.snapshot.addresses={"192.168.1.12"};
    Peer desktop;desktop.id=L"desktop";desktop.name=L"DESKTOP-02";desktop.ip="192.168.1.20";
    Peer phone;phone.id=L"phone";phone.name=L"手机浏览器";phone.ip="192.168.1.35";phone.browser=true;
    Peer notebook;notebook.id=L"notebook";notebook.name=L"NOTEBOOK-03";notebook.ip="192.168.1.28";
    app.shownPeers={desktop,phone,notebook};app.peerId=desktop.id;app.displayedPeer=L"发送至 DESKTOP-02";
    Transfer send;send.id=1;send.name=L"项目资料/a.txt";send.peer=desktop.name;send.folderId="folder";send.folderName=L"项目资料";send.folderCount=24;send.folderTotal=1374389534;send.total=1374389534;send.done=send.total*65/100;send.speed=112*1048576;send.state=State::Running;
    Transfer received;received.id=2;received.name=L"操作说明.pdf";received.peer=L"NOTEBOOK-03";received.total=3355443;received.done=received.total;received.state=State::Completed;received.receiving=true;received.savedPath=root/L"received"/L"操作说明.pdf";
    app.shownJobs=TransferRows({send,received},{});std::stable_sort(app.shownJobs.begin(),app.shownJobs.end(),[](const auto &a,const auto &b){return (a.state<State::Completed)>(b.state<State::Completed);});app.pending=9;app.pendingPartial=true;
    app.snapshot.transfers={send,received};
    SetWindowTextW(app.allButton,L"全部 (2)");SetWindowTextW(app.sentButton,L"发送 (1)");SetWindowTextW(app.receivedButton,L"接收 (1)");
    SetWindowTextW(app.request,L"来自 手机浏览器 · 2 个项目 · 86 MB");SetWindowTextW(app.accept,L"全部接受");
    SetWindowTextW(app.status,L"本机：BIGMOUNTAIN · 192.168.1.12    自动接收：关    版本 1.1");
    SetWindowTextW(app.webAddress,L"http://192.168.1.12:45871/");
    for(HWND child:{app.drop,app.filesButton,app.sendFolderButton,app.pingButton,app.noteButton})EnableWindow(child,TRUE);
    app.Layout();Capture(app,root/L"desktop.bmp");
    app.scale=1.5f;app.Fonts();bounds={0,0,1920,1200};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-150.bmp");
    app.scale=1;app.Fonts();
    SetWindowPos(window,nullptr,0,0,920,660,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();
    SetWindowTextW(app.allButton,L"全部");SetWindowTextW(app.sentButton,L"发送");SetWindowTextW(app.receivedButton,L"接收");Capture(app,root/L"desktop-compact.bmp");
    send.state=State::Rejected;send.done=0;send.speed=0;send.canRetry=true;send.savedPath=root/L"source"/L"项目资料"/L"a.txt";
    app.shownJobs=TransferRows({send,received},{});app.pending=0;app.snapshot.transfers={send,received};
    bounds={0,0,1280,800};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-retry.bmp");
    app.scale=1.5f;app.Fonts();bounds={0,0,1920,1200};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-retry-150.bmp");
    app.scale=1;app.Fonts();
    // The empty view is the affected layout: retain readable path actions at each window size.
    app.shownPeers.clear();app.shownJobs.clear();app.snapshot.transfers.clear();app.peerId.clear();app.pending=0;app.displayedPeer=L"请先选择在线设备";
    for(HWND child:{app.drop,app.filesButton,app.sendFolderButton,app.pingButton,app.noteButton})EnableWindow(child,FALSE);
    SetWindowTextW(app.allButton,L"全部 (0)");SetWindowTextW(app.sentButton,L"发送 (0)");SetWindowTextW(app.receivedButton,L"接收 (0)");
    bounds={0,0,1280,800};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-empty.bmp");
    app.scale=1.5f;app.Fonts();bounds={0,0,1920,1200};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-empty-150.bmp");
    app.scale=1;app.Fonts();SetWindowPos(window,nullptr,0,0,920,660,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"desktop-empty-compact.bmp");
    // Exercise the actual directory form against an isolated settings directory.
    app.snapshot=engine.GetSnapshot();
    const auto original=app.snapshot.config;
    WNDCLASSW modalClass{};modalClass.lpfnWndProc=Modal::Proc;modalClass.hInstance=GetModuleHandleW(nullptr);modalClass.lpszClassName=L"ChuanDirectoryCheck";modalClass.hbrBackground=app.white;RegisterClassW(&modalClass);
    auto makeDialog=[&](Modal &dialog){dialog.directory=true;return CreateWindowExW(WS_EX_DLGMODALFRAME,modalClass.lpszClassName,L"接收目录",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,0,620,300,window,nullptr,modalClass.hInstance,&dialog);};
    Modal cancelled(app,false);if(!makeDialog(cancelled))throw std::runtime_error("directory dialog creation");
    SetWindowTextW(cancelled.path,(root/L"cancelled-choice").c_str());SendMessageW(cancelled.window,WM_COMMAND,MAKEWPARAM(8,BN_CLICKED),0);
    if(!cancelled.done||engine.GetSnapshot().config.receiveDir!=original.receiveDir||fs::exists(root/L"cancelled-choice"))throw std::runtime_error("cancel must preserve the receive directory");
    DestroyWindow(cancelled.window);
    Modal edit(app,false);if(!makeDialog(edit))throw std::runtime_error("directory dialog creation");
    SetWindowTextW(edit.path,L"relative-path");edit.Save();
    if(edit.done||engine.GetSnapshot().config.receiveDir!=original.receiveDir||edit.Value(edit.help).empty())throw std::runtime_error("invalid directory must preserve settings and report an error");
    const auto chosen=root/L"新的接收目录";SetWindowTextW(edit.path,chosen.c_str());edit.Save();
    const auto updated=engine.GetSnapshot().config;const auto saved=LoadConfig(original.dataDir);
    if(!edit.done||updated.receiveDir!=chosen||saved.receiveDir!=chosen||!fs::is_directory(chosen))throw std::runtime_error("directory changes must create and persist the selected path");
    if(updated.name!=original.name||updated.autoReceive!=original.autoReceive||updated.shareSoftware!=original.shareSoftware||updated.port!=original.port||updated.maxTransferFiles!=original.maxTransferFiles||updated.maxTransferBytes!=original.maxTransferBytes)throw std::runtime_error("directory changes must preserve unrelated preferences");
    DestroyWindow(edit.window);app.Update();
    const int textLength=GetWindowTextLengthW(app.folderButton);std::wstring directoryLabel(static_cast<size_t>(textLength)+1,0);GetWindowTextW(app.folderButton,directoryLabel.data(),textLength+1);
    if(directoryLabel.find(chosen.wstring())==std::wstring::npos)throw std::runtime_error("footer path must reflect the saved directory");
    DestroyWindow(window);CoUninitialize();std::cout<<"PASS targeted native layouts and directory form: cancel, invalid-path handling, persistence and preservation of unrelated preferences\n";return 0;
}
