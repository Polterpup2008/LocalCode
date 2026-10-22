// Native process-boundary fixture, never an inference/performance result.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
int main(int argc,char** argv) {
    if (argc!=3 || std::string(argv[1])!="plugin") return 2;
#ifdef _WIN32
    const wchar_t* root=_wgetenv(L"LCD_HOME");if (!root || !*root) return 2;
    const auto home=std::filesystem::path(root);
#else
    const char* root=std::getenv("LCD_HOME");if (!root || !*root) return 2;
    const auto home=std::filesystem::u8path(root);
#endif
    const auto mode=[&] { std::ifstream file(home/"fixture-mode.txt");std::string value;std::getline(file,value);return value; };
#ifdef _WIN32
    const auto pid=GetCurrentProcessId();
#else
    const auto pid=getpid();
#endif
    std::ofstream(home/"fixture-events.log",std::ios::app)<<argv[2]<<' '<<pid<<'\n';
    const auto response=[&](int index) {
        return "{\"protocol\":1,\"ok\":true,\"result\":{\"running\":false,\"clientPid\":"+
            std::to_string(pid)+",\"index\":"+std::to_string(index)+"}}";
    };
    const std::string command=argv[2];
    if(command=="init") std::cout<<"{\"protocol\":1,\"ok\":true,\"result\":{}}\n";
    else if(command=="settings") {
        std::cout<<"{\"protocol\":1,\"ok\":true,\"result\":{";
        if(mode()!="legacy") std::cout<<"\"statusStream\":true";
        std::cout<<"}}\n";
    } else if(command=="status") std::cout<<response(1)<<'\n';
    else if(command=="status-stream") {
        int index=0;char line[16];
        while(std::cin.getline(line,sizeof(line))) {
            if(std::string(line)!="status") return 2;
            const auto current=mode();
            if(current=="delay") std::this_thread::sleep_for(std::chrono::seconds(2));
            if(current=="exit") return 3;
            if(current=="bad-json") std::cout<<"{invalid\n";
            else if(current=="oversized") std::cout<<std::string(17*1024,'x')<<'\n';
            else if(current=="double") std::cout<<response(++index)<<'\n'<<response(++index)<<'\n';
            else if(current=="partial") std::cout<<"{\"protocol\":";
            else std::cout<<response(++index)<<'\n';
            std::cout.flush();
        }
    } else return 2;
    return 0;
}
