#include "core.h"
#include <iostream>
int wmain(int argc,wchar_t **argv){
    if(argc!=5)return 2;
    chuan::Config config=chuan::LoadConfig(chuan::fs::path(argv[1])/L"config");config.receiveDir=chuan::fs::path(argv[1])/L"received";config.port=static_cast<unsigned short>(std::stoi(argv[2]));config.discoveryPort=static_cast<unsigned short>(std::stoi(argv[3]));config.zipFolders=true;
    chuan::Engine engine(config);std::wstring error;if(!engine.Start(error))return 3;bool sent=false;
    const auto deadline=GetTickCount64()+45000;
    while(GetTickCount64()<deadline){auto state=engine.GetSnapshot();if(!sent){for(const auto &peer:state.peers)if(peer.browser){engine.SendFiles(peer,{chuan::fs::path(argv[4])});sent=true;break;}}
        // Keep the same authenticated source available for range and prefix checks.
        Sleep(20);
    }return 5;
}
