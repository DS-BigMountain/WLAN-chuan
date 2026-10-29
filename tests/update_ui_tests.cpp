#define CHUAN_UI_TEST_BUILD
#include "../src/app.cpp"
#include <stdexcept>
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void Capture(HWND window, const fs::path &path) {
    RECT rect{}; GetClientRect(window, &rect); const int w = rect.right, h = rect.bottom;
    HDC screen = GetDC(window), dc = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h); auto old = SelectObject(dc, bitmap);
    PrintWindow(window, dc, PW_CLIENTONLY); SelectObject(dc, old);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    std::vector<char> data(static_cast<size_t>(w) * h * 4); GetDIBits(dc, bitmap, 0, h, data.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader); header.bfSize = header.bfOffBits + static_cast<DWORD>(data.size());
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<char *>(&header), sizeof(header)); out.write(reinterpret_cast<char *>(&info.bmiHeader), sizeof(info.bmiHeader)); out.write(data.data(), data.size());
    DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window, screen);
}
int wmain(int argc, wchar_t **argv) {
    try {
        Check(argc == 2, "output directory required"); SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
        const auto root = fs::absolute(argv[1]); fs::create_directories(root); auto config = LoadConfig(root / L"config"); Engine engine(config); App app(engine);
        WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"ChuanUpdateUiTest"; RegisterClassW(&cls);
        HWND main = CreateWindowW(cls.lpszClassName, L"更新界面验证", WS_OVERLAPPEDWINDOW, 0, 0, 1100, 800, nullptr, nullptr, cls.hInstance, &app); Check(main != nullptr, "main window");
        WNDCLASSW modal{}; modal.lpfnWndProc = Modal::Proc; modal.hInstance = cls.hInstance; modal.lpszClassName = L"ChuanUpdateModalTest"; modal.hbrBackground = app.white; RegisterClassW(&modal);
        for(float scale : {1.f,1.5f}) {
            app.scale = scale; app.Fonts();
            for(bool updating : {false,true}) {
                Modal dialog(app, !updating); dialog.updating = updating;
                RECT bounds{0, 0, app.Px(updating ? 560.f : 470.f), app.Px(updating ? 410.f : 562.f)};
                AdjustWindowRectEx(&bounds, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_DLGMODALFRAME);
                HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, modal.lpszClassName, updating ? L"在线更新" : L"设置", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, bounds.right-bounds.left,bounds.bottom-bounds.top,main,nullptr,modal.hInstance,&dialog);
                Check(window != nullptr && GetDlgItem(window,17) != nullptr,"update entry"); ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
                RECT client{};GetClientRect(window,&client);
                struct BoundsCheck { RECT client; bool inside = true; } boundsCheck{client};
                EnumChildWindows(window,[](HWND child,LPARAM raw)->BOOL{auto check=reinterpret_cast<BoundsCheck *>(raw);RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,GetParent(child),reinterpret_cast<POINT *>(&r),2);if(r.left<0||r.top<0||r.right>check->client.right||r.bottom>check->client.bottom)check->inside=false;return TRUE;},reinterpret_cast<LPARAM>(&boundsCheck));
                Check(boundsCheck.inside,"dialog controls extend outside client area");
                if(updating){
                    Check(IsWindowEnabled(dialog.updateAction),"initial action enabled");Check(!IsWindowEnabled(dialog.updateCancel),"initial cancellation disabled");Check(dialog.Value(dialog.help).find(L"V"+std::wstring(Version))!=std::wstring::npos,"current version visible");
                    for(const auto phase:{update::Phase::Idle,update::Phase::Checking,update::Phase::Current,update::Phase::Available,update::Phase::Ready,update::Phase::Error}){
                        update::Status state;state.phase=phase;dialog.UpdateStatus(&state);
                        Check((GetWindowLongW(dialog.updateProgress,GWL_STYLE)&WS_VISIBLE)==0,"progress visible before download or after completion");
                    }
                    update::Status state;state.phase=update::Phase::Downloading;state.done=50;state.total=100;dialog.UpdateStatus(&state);
                    Check((GetWindowLongW(dialog.updateProgress,GWL_STYLE)&WS_VISIBLE)!=0,"progress missing during download");
                    Check(SendMessageW(dialog.updateProgress,PBM_GETPOS,0,0)==50,"download progress value");
                    dialog.UpdateStatus();
                }
                Capture(window,root/((updating?L"windows-update-":L"windows-settings-")+std::to_wstring(static_cast<int>(scale*100))+L".bmp"));DestroyWindow(window);
            }
        }
        DestroyWindow(main); CoUninitialize(); std::cout<<"PASS Windows settings and update dialogs at 100% and 150%\n";return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
