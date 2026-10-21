#define CHUAN_UI_TEST_BUILD
#include "../src/app.cpp"
#include <stdexcept>

struct ListChanges { unsigned resets = 0, heights = 0, redraws = 0; };
static LRESULT CALLBACK ObserveList(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto &changes = *reinterpret_cast<ListChanges *>(data);
    changes.resets += message == LB_RESETCONTENT;
    changes.heights += message == LB_SETITEMHEIGHT;
    changes.redraws += message == WM_SETREDRAW;
    return DefSubclassProc(window, message, w, l);
}
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }

int wmain(int argc, wchar_t **argv) { try {
    Check(argc == 2, "output directory required");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto config = LoadConfig(fs::absolute(argv[1]));
    Engine engine(config); App app(engine);
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"ChuanTransferRedrawTest"; RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"Transfer redraw", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        0, 0, 1440, 1000, nullptr, nullptr, cls.hInstance, &app);
    Check(window != nullptr, "window creation"); KillTimer(window, 1);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ListChanges changes; SetWindowSubclass(app.tasks, ObserveList, 42, reinterpret_cast<DWORD_PTR>(&changes));
    for (bool receiving : {true, false}) {
        Snapshot sample = engine.GetSnapshot();
        for (int i = 0; i < 6; ++i) {
            Transfer job; job.id = i + 1; job.name = (i < 3 ? L"资料甲\\" : L"资料乙\\") + std::to_wstring(i) + L".bin";
            job.folderId = i < 3 ? "a" : "b"; job.folderName = i < 3 ? L"资料甲" : L"资料乙";
            job.peer = L"测试设备"; job.receiving = receiving; job.total = 100; job.folderTotal = 300;
            job.folderCount = 3; job.state = State::Running; sample.transfers.push_back(job);
        }
        for (bool expanded : {false, true}) {
            for (auto &job : sample.transfers) { job.state = State::Running; job.done = 0; }
            app.expandedFolders.clear();
            if (expanded) for (const auto &job : sample.transfers) app.expandedFolders.insert(TransferGroup(job));
            app.Update(&sample);
            std::vector<uint64_t> ids; for (const auto &row : app.shownJobs) ids.push_back(row.id);
            app.selectedTask = app.shownJobs.back().id; app.Update(&sample);
            SendMessageW(app.tasks, LB_SETTOPINDEX, 1, 0);
            const auto top = SendMessageW(app.tasks, LB_GETTOPINDEX, 0, 0);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            changes = {};
            for (int i = 0; i < 2; ++i) {
                sample.transfers[i].state = State::Completed; sample.transfers[i].done = 100;
                app.Update(&sample);
                Check(changes.resets == 0 && changes.heights == 0 && changes.redraws == 0,
                    "member completion must not rebuild list, change heights or suspend redraw");
                Check(GetUpdateRect(app.tasks, nullptr, FALSE), "completion invalidates affected rows");
                Check(!GetUpdateRect(window, nullptr, FALSE), "completion does not invalidate parent window");
                for (size_t index = 0; index < ids.size(); ++index)
                    Check(app.shownJobs[index].id == ids[index], "member completion preserves folder and child order");
                const auto folder=std::find_if(app.shownJobs.begin(),app.shownJobs.end(),[](const auto &row){return row.folderRow&&row.name==L"资料甲";});
                Check(folder!=app.shownJobs.end()&&folder->currentFile==std::to_wstring(i+1)+L".bin",
                    "current file advances past completed members");
                Check(folder->currentAction==(receiving?L"接收文件":L"发送文件"),"direction-specific current action");
                Check(SendMessageW(app.tasks, LB_GETTOPINDEX, 0, 0) == top, "scroll position retained");
                Check(app.selectedTask == ids.back(), "selection retained");
                UpdateWindow(app.tasks);
            }
            sample.transfers[2].state = State::Failed; sample.transfers[2].error = L"读取失败";
            app.Update(&sample); UpdateWindow(app.tasks); changes = {};
            sample.transfers[2].state = State::Cancelled; sample.transfers[2].error.clear();
            app.Update(&sample);
            Check(!changes.resets && GetUpdateRect(app.tasks, nullptr, FALSE), "state and count changes repaint in place");
            sample.transfers[3].state=State::Queued;sample.transfers[3].activity=L"正在校验已有文件";
            app.Update(&sample);UpdateWindow(app.tasks);changes={};
            auto current=std::find_if(app.shownJobs.begin(),app.shownJobs.end(),[](const auto &row){return row.folderRow&&row.name==L"资料乙";});
            Check(current!=app.shownJobs.end()&&current->currentFile==L"3.bin"&&current->currentAction==L"核对同名文件",
                "existing-file verification is visible before payload reception");
            sample.transfers[3].activity.clear();sample.transfers[4].activity=L"正在保存文件";
            app.Update(&sample);
            current=std::find_if(app.shownJobs.begin(),app.shownJobs.end(),[](const auto &row){return row.folderRow&&row.name==L"资料乙";});
            Check(current->currentFile==L"4.bin"&&current->currentAction==L"写入磁盘","phase switches to current processing file");
            Check(!changes.resets&&GetUpdateRect(app.tasks,nullptr,FALSE),"file and phase changes repaint without list rebuild");
            for(auto &job:sample.transfers){job.state=State::Completed;job.done=job.total;}
            app.Update(&sample);
            for(const auto &row:app.shownJobs)if(row.folderRow)Check(row.currentFile.empty()&&row.currentAction.empty(),"ended folder has no stale current file");
            for(auto &job:sample.transfers){job.state=State::Queued;job.done=0;job.activity.clear();}app.Update(&sample);
            for(const auto &row:app.shownJobs)if(row.folderRow){Check(row.currentFile.empty()&&row.currentCount==0,"queued files are never presented as processing");Check(row.currentAction.find(L"等待处理")==std::wstring::npos,"waiting names its actual dependency");}
        }
    }
    RemoveWindowSubclass(app.tasks, ObserveList, 42); DestroyWindow(window); CoUninitialize();
    std::cout << "PASS sending/receiving folder completion, collapsed/expanded rows, state repaint, order, selection and scrolling\n";
    return 0;
} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; } }
