#pragma once
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#if defined(__APPLE__)
#include <charconv>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <unistd.h>
// Only app-supervised helpers receive this variable. Direct developer CLI runs
// retain their normal lifetime. Reparenting cannot be confused with PID reuse.
inline void aetherGuardParent()
{
    const char* value=std::getenv("AETHER_PARENT_PID");
    if (!value) return;
    int expected=0;const auto end=value+std::char_traits<char>::length(value);
    const auto parsed=std::from_chars(value,end,expected);
    if (parsed.ec!=std::errc{} || parsed.ptr!=end || expected<=1) _exit(125);
    if (getppid()!=expected) _exit(125);
    try
    {
        std::thread([expected] {
            for (;;)
            {
                if (getppid()!=expected) _exit(125);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    catch (...) {_exit(125);}
}
#endif
inline std::filesystem::path aetherUtf8Path(const char* text)
{
    std::u8string value;
    for (char byte:std::string_view(text)) value.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path(value);
}
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdexcept>
// Console argv is otherwise encoded using the active Windows code page.
// Keep all native helper arguments UTF-8, including Unicode user directories.
inline int aetherUnicodeEntry(int argc,wchar_t** argv,int (*body)(int,char**))
{
    try
    {
        std::vector<std::string> storage;storage.reserve(static_cast<size_t>(argc));
        for (int i=0;i<argc;++i)
        {
            const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
            if (bytes<1) throw std::runtime_error("Invalid Unicode helper argument");
            std::string value(static_cast<size_t>(bytes),'\0');
            if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),bytes,nullptr,nullptr)) throw std::runtime_error("Invalid Unicode helper argument");
            value.pop_back();storage.push_back(std::move(value));
        }
        std::vector<char*> arguments;arguments.reserve(storage.size()+1);
        for (auto& value:storage) arguments.push_back(value.data());arguments.push_back(nullptr);
        return body(argc,arguments.data());
    }
    catch (...) {return 1;}
}
#define AETHER_NATIVE_ENTRY(body) int wmain(int argc,wchar_t** argv) {return aetherUnicodeEntry(argc,argv,body);}
#elif defined(__APPLE__)
#define AETHER_NATIVE_ENTRY(body) int main(int argc,char** argv) {aetherGuardParent();return body(argc,argv);}
#else
#define AETHER_NATIVE_ENTRY(body) int main(int argc,char** argv) {return body(argc,argv);}
#endif
