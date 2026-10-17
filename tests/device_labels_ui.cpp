// Reuse the established native renderer capture helpers for the new device badges.
#define wmain FolderFixtureMain
#include "folder_tests.cpp"
#undef wmain
int wmain(int argc,wchar_t **argv){try{
    Check(argc==2,"output directory required");SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES};InitCommonControlsEx(&common);
    const auto root=fs::absolute(argv[1]);fs::create_directories(root);auto config=LoadConfig(root/L"config");config.receiveDir=root/L"received";Engine engine(config);App app(engine);
    WNDCLASSW cls{};cls.lpfnWndProc=WindowProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ChuanDeviceLabelsTest";RegisterClassW(&cls);HWND parent=CreateWindowW(cls.lpszClassName,L"Device labels",WS_OVERLAPPEDWINDOW,0,0,1280,800,nullptr,nullptr,cls.hInstance,&app);Check(parent!=nullptr,"create native UI");KillTimer(parent,1);
    auto sample=engine.GetSnapshot();for(const auto &kind:{"windows","android","desktop-web","mobile-web"}){Peer peer;peer.id=L"device-"+std::to_wstring(sample.peers.size()+1);peer.name=L"A"+std::to_wstring(sample.peers.size()+1);peer.ip="192.168.31."+std::to_string(170+sample.peers.size());peer.browser=std::string(kind).ends_with("web");peer.platform=kind;sample.peers.push_back(peer);}
    app.Update(&sample);Check(app.shownPeers.size()==4,"all device categories visible");
    for(const auto scale:{1.f,1.5f}){app.scale=scale;app.Fonts();RECT size{0,0,static_cast<LONG>(1280*scale),static_cast<LONG>(900*scale)};AdjustWindowRectEx(&size,WS_OVERLAPPEDWINDOW,FALSE,0);SetWindowPos(parent,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Update(&sample);app.Layout();Capture(app,root/(scale==1.f?L"device-labels.bmp":L"device-labels-150.bmp"));}
    DestroyWindow(parent);CoUninitialize();std::cout<<"PASS Windows device category badges at 100% and 150%\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
