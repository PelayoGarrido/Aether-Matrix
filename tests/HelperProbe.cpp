#include "../Source/NativeEntry.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <fcntl.h>
#endif
static int runNative(int argc,char** argv)
{
    const std::string mode=argc>1 ? argv[1] : "";
    if (mode=="fail") return 7;
    if (mode=="sleep") {std::signal(SIGTERM,[](int){});std::cout<<"ready"<<std::endl;std::this_thread::sleep_for(std::chrono::seconds(30));return 0;}
#if defined(__APPLE__)
    if (mode=="fd" && argc==3) return fcntl(std::stoi(argv[2]),F_GETFD)<0 ? 0 : 33;
#endif
    for (int i=1;i<argc;++i) std::cout<<argv[i]<<'\n';
    return 0;
}

AETHER_NATIVE_ENTRY(runNative)
