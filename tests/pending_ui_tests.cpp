// Directed native grouping, item selection and layout regression with existing transfer history.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "core.h"
#undef private
#define wmain FolderFixtureMain
#include "folder_tests.cpp"
#undef wmain
int wmain(int argc,wchar_t **argv){try{
    Check(argc==2,"output directory required");SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES};InitCommonControlsEx(&common);
    const auto root=fs::absolute(argv[1]);fs::create_directories(root);auto config=LoadConfig(root/L"config");config.receiveDir=root/L"received";config.autoReceive=false;Engine engine(config);App app(engine);
    auto &source=engine.browsers_["source"];source.id=L"web:source";source.openTabs.insert("tab");source.tabs["tab"]=GetTickCount64();
    WNDCLASSW cls{};cls.lpfnWndProc=WindowProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ChuanPendingUiTest";RegisterClassW(&cls);HWND parent=CreateWindowW(cls.lpszClassName,L"Pending receive requests",WS_OVERLAPPEDWINDOW,-30000,-30000,1280,900,nullptr,nullptr,cls.hInstance,&app);Check(parent!=nullptr,"create native UI");KillTimer(parent,1);
    const auto add=[&](int id,const wchar_t *peer,int count){auto job=std::make_shared<Engine::Job>();job->view.id=id;job->view.name=L"文件批次";job->view.peer=peer;job->view.receiving=true;job->view.requestOnly=true;job->view.awaitingApproval=true;job->view.state=State::Waiting;job->webClient="source";job->webTab="tab";job->asyncOfferKey="pending-"+std::to_string(id);for(int i=0;i<count;++i)job->view.offerItems.push_back({L"待接收文件-"+std::to_wstring(id)+L"-"+std::to_wstring(i)+L".txt",false,1,1024});engine.batchApprovals_[job->asyncOfferKey].items=job->view.offerItems;engine.jobs_.push_back(job);return job;};
    auto a=add(1,L"A1 (192.168.1.10)",3),b=add(2,L"A2 (192.168.1.20)",1);app.Update();Check(app.approvals.rows.size()==6&&app.approvals.All(),"two sender headers and four checked file children");
    app.approvals.checked[{1,1}]=false;app.approvals.collapsed.insert(a->view.peer);auto c=add(3,a->view.peer.c_str(),1);app.Update();Check(app.approvals.checked.at({1,1})==false&&app.approvals.checked.at({3,0}),"manual unchecked state persists and new file is checked");Check(app.approvals.collapsed.contains(a->view.peer),"sender expansion state persists");
    app.Command(SelectAllRequests,BN_CLICKED);Check(app.approvals.All(),"global all checks every sender including collapsed files");app.Command(SelectAllRequests,BN_CLICKED);Check(app.approvals.Selected()==0,"second global click clears every sender");
    app.approvals.checked[{1,1}]=true;app.approvals.checked[{2,0}]=true;app.RefreshPending();app.Command(Accept,BN_CLICKED);Check(a->view.awaitingApproval&&a->selection=="-1-"&&b->view.state==State::Completed,"checked file accepted while same batch stays pending");Check(c->view.awaitingApproval&&app.approvals.leaves.size()==3,"another new batch remains pending");
    app.approvals.collapsed.clear();app.RefreshPending();
    auto history=std::make_shared<Engine::Job>();history->view.id=99;history->view.name=L"Screenshot_2026-10-17-20-48-50-long-name.jpg";history->view.peer=L"A1";history->view.total=1048576;history->view.done=1048576;history->view.state=State::Completed;engine.jobs_.push_back(history);app.Update();
    for(const auto scale:{1.f,1.5f})for(const auto size:{std::pair{1280,800},std::pair{1000,640},std::pair{1280,900}}){app.scale=scale;app.Fonts();RECT bounds{0,0,app.Px(float(size.first)),app.Px(float(size.second))};AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);SetWindowPos(parent,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);app.Layout();
        RECT list{},tasks{},footer{},accept{};GetWindowRect(app.requestList,&list);GetWindowRect(app.tasks,&tasks);GetWindowRect(app.folderButton,&footer);GetWindowRect(app.accept,&accept);Check(accept.bottom<=list.top,"toolbar above pending children");Check(list.bottom<tasks.top&&tasks.bottom<footer.top,"approval, completed history and footer do not overlap");
        for(HWND button:{app.filesButton,app.sendFolderButton}){Check(GetWindowLongW(button,GWL_STYLE)&WS_VISIBLE,"both selectors always visible");RECT rect{};GetWindowRect(button,&rect);POINT point{(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};ScreenToClient(parent,&point);Check(ChildWindowFromPointEx(parent,point,CWP_SKIPINVISIBLE)==button,"selectors remain above drop surface");}
        Check(GetWindowLongW(app.drop,GWL_STYLE)&WS_CLIPSIBLINGS,"drop surface clips sibling selector windows");RedrawWindow(parent,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        Capture(app,root/(L"grouped-"+std::to_wstring(size.first)+L"-"+std::to_wstring(size.second)+(scale==1.f?L"-100.bmp":L"-150.bmp")));
    }
    app.approvals.ToggleAll();app.RefreshPending();app.Command(Reject,BN_CLICKED);Check(!a->view.awaitingApproval&&a->selection=="010"&&c->view.state==State::Rejected,"reject changes only remaining checked items");
    DestroyWindow(parent);CoUninitialize();std::cout<<"PASS native grouped approval, checked arrivals, global toggle, partial consent, pending retention, completed-history layout and persistent selector z-order at 100%/150%\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
