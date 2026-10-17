// Native inline hierarchy, row states and layout checks using isolated fixtures.
#define CHUAN_UI_TEST_BUILD
#include "../src/app.cpp"
#include <stdexcept>
static void Check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
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
        if(item>=0){RECT itemBounds{};SendMessageW(child,LB_GETITEMRECT,item,reinterpret_cast<LPARAM>(&itemBounds));MapWindowPoints(child,app.window,reinterpret_cast<POINT *>(&itemBounds),2);RECT clipped{};if(!IntersectRect(&clipped,&bounds,&itemBounds))return;bounds=clipped;}
        if(bounds.bottom<=bounds.top)return;
        DRAWITEMSTRUCT draw{};draw.CtlID=id;draw.hwndItem=child;draw.itemID=item<0?0:static_cast<UINT>(item);
        if(!IsWindowEnabled(child))draw.itemState|=ODS_DISABLED;
        if(id==Devices&&draw.itemID<app.shownPeers.size()&&app.shownPeers[draw.itemID].id==app.peerId)draw.itemState|=ODS_SELECTED;
        if(id==PendingRequests&&draw.itemID<app.pendingJobs.size()&&app.pendingJobs[draw.itemID].id==app.pending)draw.itemState|=ODS_SELECTED;
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
    if(app.pending){for(size_t i=0;i<app.approvals.rows.size();++i)control(app.requestList,PendingRequests,static_cast<int>(i),1);control(app.reject,Reject,-1,1);control(app.selectAllRequests,SelectAllRequests,-1,1);control(app.accept,Accept,-1,1);}
    for(HWND child:{app.request,app.status,app.webAddress}) {
        if(child==app.request&&!app.pending)continue;
        if(child==app.webAddress&&app.width<1160)continue;
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT *>(&bounds),2);
        wchar_t text[512]{};GetWindowTextW(child,text,512);
        app.Buffered(dc,bounds,1,[&](ui::Canvas &canvas,float w,float h){canvas.BoxColor(0,0,w,h,0,child==app.request?PendingSurface:child==app.webAddress?D2D1::ColorF(0xF9FAFC):D2D1::ColorF(0xF9F9F9));canvas.TextColor(text,0,0,w,h,app.captionFormat,TextMuted);});
    }
    SelectObject(dc,old);SaveBitmap(dc,bitmap,width,height,path);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(app.window,screen);
}
int wmain(int argc,wchar_t **argv){try{
    Check(argc==3,"manifest and output directory required");SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES};InitCommonControlsEx(&common);
    const auto root=fs::absolute(argv[2]);fs::create_directories(root);std::ifstream manifest(argv[1]);std::vector<Transfer> jobs;std::string line;
    uint64_t total=0;while(std::getline(manifest,line)){const auto tab=line.find('\t');Check(tab!=std::string::npos,"manifest fields");Transfer job;job.id=jobs.size()+1;job.name=Wide(Unhex(line.substr(0,tab)));job.total=std::stoull(line.substr(tab+1));job.receiving=true;job.folderId="ht-fixture";job.folderName=L"HT";job.peer=L"浏览器 A1";job.state=State::Cancelled;total+=job.total;jobs.push_back(job);}
    Check(jobs.size()==1490,"HT fixture count");std::vector<size_t> order;for(size_t i=0;i<jobs.size();++i)order.push_back(i);std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return jobs[a].total<jobs[b].total;});
    for(size_t i=0;i<92;++i){auto &job=jobs[order[i]];job.state=State::Completed;job.done=job.total;}
    auto &failed=jobs[order[92]];failed.state=State::Failed;failed.error=L"测试注入：发送文件读取失败";
    for(auto &job:jobs){job.folderCount=1490;job.folderTotal=total;}
    const auto rows=TransferRows(jobs,{});Check(rows.size()==1,"one folder group");const auto &row=rows[0];Check(row.name==L"HT"&&row.state==State::Failed&&row.completedFiles==92&&row.failedFiles==1,"receiver partial result and counts");
    Check(row.done>0&&row.total==19685133076ull&&row.done<row.total,"receiver retains actual bytes");const auto model=FolderTree(jobs);Check(model.size()>1490,"directory hierarchy");
    const auto failedNode=std::find_if(model.begin(),model.end(),[&](const auto &node){return node.path==failed.name;});Check(failedNode!=model.end()&&failedNode->error==failed.error,"individual failure in tree");
    auto config=LoadConfig(root/L"config");config.receiveDir=root/L"received";Engine engine(config);App app(engine);
    WNDCLASSW appClass{};appClass.lpfnWndProc=WindowProc;appClass.hInstance=GetModuleHandleW(nullptr);appClass.lpszClassName=L"ChuanFolderModelTest";RegisterClassW(&appClass);
    HWND parent=CreateWindowW(appClass.lpszClassName,L"Chuan test",WS_OVERLAPPEDWINDOW,0,0,1280,800,nullptr,nullptr,appClass.hInstance,&app);Check(parent!=nullptr,"parent creation");KillTimer(parent,1);
    Snapshot sample=engine.GetSnapshot();sample.transfers=jobs;app.Update(&sample);Check(app.shownJobs.size()==1,"collapsed folder is one transfer item");
    const auto group=app.shownJobs.front().group;app.expandedFolders.insert(group);for(const auto &node:model)if(node.directory)app.expandedFolders.insert(group+"|"+Utf8(node.path));app.Update(&sample);
    Check(app.shownJobs.size()==model.size(),"all nested files and directories remain in the original task list");Check(SendMessageW(app.tasks,LB_GETCOUNT,0,0)==static_cast<LRESULT>(model.size()),"native list contains the inline hierarchy");
    const auto failedRow=std::find_if(app.shownJobs.begin(),app.shownJobs.end(),[&](const auto &item){return item.id==failed.id;});Check(failedRow!=app.shownJobs.end()&&failedRow->childRow&&failedRow->error==failed.error,"file row retains precise failure");
    Check(GetWindowLongW(app.tasks,GWL_STYLE)&LBS_OWNERDRAWVARIABLE,"compact child rows use independent heights");
    app.expandedFolders.clear();app.Update(&sample);Check(app.shownJobs.size()==1,"collapse restores the original item");
    std::vector<Transfer> preview;const wchar_t *names[]={L"HT\\产品资料\\使用说明.pdf",L"HT\\产品资料\\设计图.png",L"HT\\产品资料\\演示视频.mp4",L"HT\\安装程序.exe"};
    for(int i=0;i<4;i++){Transfer item;item.id=i+1;item.name=names[i];item.peer=L"A1 (192.168.31.170)";item.folderName=L"HT";item.folderId="preview";item.receiving=true;item.total=(i+1)*1048576;item.folderTotal=10*1048576;item.folderCount=4;item.state=i==0?State::Completed:i==1?State::Failed:i==2?State::Running:State::Queued;item.done=i==0?item.total:i==2?item.total/2:0;item.speed=i==2?524288:0;if(i==1)item.error=L"源文件读取失败，请检查文件后重试";preview.push_back(item);}
    sample.transfers=preview;const auto previewGroup=TransferGroup(preview.front());app.expandedFolders={previewGroup,previewGroup+"|"+Utf8(L"HT\\产品资料")};
    RECT size{0,0,1280,1000};AdjustWindowRectEx(&size,WS_OVERLAPPEDWINDOW,FALSE,0);SetWindowPos(parent,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Update(&sample);app.Layout();Capture(app,root/L"native-inline-folder.bmp");
    Check(app.shownJobs.size()==6,"expanded preview has parent, directory and four file rows");Check(SendMessageW(app.tasks,LB_GETITEMHEIGHT,2,0)<SendMessageW(app.tasks,LB_GETITEMHEIGHT,0,0),"file rows are more compact than parent");
    app.scale=1.5f;app.Fonts();size={0,0,1920,1500};AdjustWindowRectEx(&size,WS_OVERLAPPEDWINDOW,FALSE,0);SetWindowPos(parent,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();Capture(app,root/L"native-inline-folder-150.bmp");
    DestroyWindow(parent);CoUninitialize();std::cout<<"PASS native inline HT hierarchy: 1490 files, row states, collapse, compact heights, progress and 150% layout\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
