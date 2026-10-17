#include "NativeProcess.h"
#include <chrono>
#include <array>
#include <vector>
#include <stdexcept>
#include <thread>
#if JUCE_MAC
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/machine.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace aether
{
juce::File NativeProcess::helpers()
{
#if JUCE_MAC
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&NativeProcess::helpers),&info)) return {};
    return juce::File(juce::String::fromUTF8(info.dli_fname)).getParentDirectory().getSiblingFile("Helpers");
#elif JUCE_WINDOWS
    HMODULE module=nullptr;std::array<wchar_t,32768> path{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&NativeProcess::helpers),&module)) return {};
    const auto size=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if (!size || size==path.size()) return {};
    return juce::File(juce::String(path.data())).getParentDirectory().getChildFile("Helpers");
#else
    return {};
#endif
}
void NativeProcess::run(const juce::File& executable,const juce::StringArray& arguments,
    const juce::File& log,int seconds,const std::atomic<bool>& cancel,const std::function<void()>& poll)
{
    using Clock=std::chrono::steady_clock;
    if (executable.isSymbolicLink() || !executable.existsAsFile() || log.exists() || log.isSymbolicLink() || seconds<1 || seconds>180)
        throw std::runtime_error("Native helper or private job log is unavailable");
    if (cancel.load()) throw std::runtime_error("Piano job cancelled");
    const auto start=Clock::now();
#if JUCE_MAC
    struct Setup
    {
        int output=-1;posix_spawn_file_actions_t actions{};posix_spawnattr_t attributes{};bool actionReady=false,attributesReady=false;
        void closeAll() noexcept {if (actionReady) {posix_spawn_file_actions_destroy(&actions);actionReady=false;}if (attributesReady) {posix_spawnattr_destroy(&attributes);attributesReady=false;}if (output>=0) {close(output);output=-1;}}
        ~Setup(){closeAll();}
    } setup;
    setup.output=open(log.getFullPathName().toRawUTF8(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if (setup.output<0) throw std::runtime_error("Cannot create private helper log");
    setup.actionReady=posix_spawn_file_actions_init(&setup.actions)==0;
    setup.attributesReady=posix_spawnattr_init(&setup.attributes)==0;
    auto& actions=setup.actions;auto& attributes=setup.attributes;const int output=setup.output;
    int nativeArm=0;size_t armSize=sizeof(nativeArm);sysctlbyname("hw.optional.arm64",&nativeArm,&armSize,nullptr,0);
    cpu_type_t cpu=nativeArm ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;size_t accepted=0;
    sigset_t empty,defaults;sigemptyset(&empty);sigemptyset(&defaults);sigaddset(&defaults,SIGINT);sigaddset(&defaults,SIGTERM);
    if (!setup.actionReady || !setup.attributesReady
        || posix_spawnattr_setbinpref_np(&attributes,1,&cpu,&accepted) || accepted!=1
        || posix_spawn_file_actions_addchdir_np(&actions,log.getParentDirectory().getFullPathName().toRawUTF8())
        || posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0)
        || posix_spawn_file_actions_adddup2(&actions,output,STDOUT_FILENO)
        || posix_spawn_file_actions_adddup2(&actions,output,STDERR_FILENO)
        || posix_spawnattr_setsigmask(&attributes,&empty)
        || posix_spawnattr_setsigdefault(&attributes,&defaults)
        || posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF))
    {throw std::runtime_error("Cannot prepare isolated helper process");}
    std::vector<std::string> storage{executable.getFullPathName().toStdString()};
    for (const auto& argument:arguments) storage.push_back(argument.toStdString());
    std::vector<char*> argv;for (auto& argument:storage) argv.push_back(argument.data());argv.push_back(nullptr);
    // No inherited tokens, dynamic-loader overrides or shell configuration.
    std::vector<std::string> environment{"PATH=/usr/bin:/bin","LANG=en_US.UTF-8","ORT_DISABLE_TELEMETRY=1",
        "AETHER_PARENT_PID="+std::to_string(getpid()),
        "HOME="+juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString(),
        "TMPDIR="+juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString()};
    std::vector<char*> env;for (auto& value:environment) env.push_back(value.data());env.push_back(nullptr);
    pid_t pid=-1;const int created=posix_spawn(&pid,argv[0],&actions,&attributes,argv.data(),env.data());setup.closeAll();
    if (created) throw std::runtime_error("Could not start bundled piano helper");
    bool reaped=false;
    auto reap=[&] {
        if (reaped) return;
        int existing=0;const auto owned=waitpid(pid,&existing,WNOHANG);
        if (owned==pid || (owned<0 && errno==ECHILD)) {reaped=true;return;}
        kill(pid,SIGTERM);
        const auto until=Clock::now()+std::chrono::milliseconds(500);int status=0;
        while (Clock::now()<until)
        {
            const auto result=waitpid(pid,&status,WNOHANG);
            if (result==pid || (result<0 && errno==ECHILD)) {reaped=true;return;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid,SIGKILL);while (waitpid(pid,&status,0)<0 && errno==EINTR) {}reaped=true;
    };
    try
    {
        while (true)
        {
            int status=0;const auto result=waitpid(pid,&status,WNOHANG);
            if (result==pid)
            {
                reaped=true;
                if (!WIFEXITED(status) || WEXITSTATUS(status)!=0) throw std::runtime_error("Piano helper failed; diagnostics retained with this job");
                break;
            }
            if (result<0 && errno!=EINTR) {if (errno==ECHILD) reaped=true;throw std::runtime_error("Piano helper status unavailable");}
            proc_taskinfo memory{};
            if (proc_pidinfo(pid,PROC_PIDTASKINFO,0,&memory,sizeof(memory))==sizeof(memory)
                && memory.pti_resident_size>2ULL*1024*1024*1024) throw std::runtime_error("Piano helper exceeds 2 GiB sampled RSS");
            if (cancel.load() || Clock::now()-start>std::chrono::seconds(seconds)) throw std::runtime_error("Piano job cancelled or timed out");
            if (log.getSize()>2*1024*1024 || log.getParentDirectory().getBytesFreeOnVolume()<3LL*1024*1024*1024)
                throw std::runtime_error("Piano job disk reserve reached");
            if (poll) poll();std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    catch (...) {reap();throw;}
#elif JUCE_WINDOWS
    struct Handle {HANDLE value=nullptr;~Handle(){if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}};
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    Handle output{CreateFileW(log.getFullPathName().toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
    Handle input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if (output.value==INVALID_HANDLE_VALUE || input.value==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open private helper log");
    auto quote=[](const juce::String& value) {
        std::wstring out=L"\"";size_t slashes=0;
        for (wchar_t c:std::wstring(value.toWideCharPointer()))
        {
            if (c==L'\\') {++slashes;continue;}
            out.append(slashes*(c==L'\"' ? 2 : 1),L'\\');slashes=0;
            if (c==L'\"') out.push_back(L'\\');out.push_back(c);
        }
        out.append(slashes*2,L'\\');out.push_back(L'\"');return out;
    };
    std::wstring command=quote(executable.getFullPathName());for (const auto& a:arguments) command+=L" "+quote(a);
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> attributes(bytes);auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list,1,0,&bytes)) throw std::runtime_error("Cannot prepare helper handles");
    HANDLE inherited[]{input.value,output.value};
    if (!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))
    {DeleteProcThreadAttributeList(list);throw std::runtime_error("Cannot restrict helper handles");}
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=input.value;
    startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=output.value;
    std::wstring environment=L"SystemRoot="+std::wstring(juce::SystemStats::getEnvironmentVariable("SystemRoot","C:\\Windows").toWideCharPointer());environment.push_back(0);
    environment+=L"TEMP="+std::wstring(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toWideCharPointer());environment.push_back(0);environment.push_back(0);
    PROCESS_INFORMATION child{};
    const auto created=CreateProcessW(executable.getFullPathName().toWideCharPointer(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,
        environment.data(),log.getParentDirectory().getFullPathName().toWideCharPointer(),&startup.StartupInfo,&child);
    DeleteProcThreadAttributeList(list);
    if (!created) throw std::runtime_error("Could not start bundled piano helper");
    Handle process{child.hProcess},thread{child.hThread},job{CreateJobObjectW(nullptr,nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit=1;
    limits.ProcessMemoryLimit=2ULL*1024*1024*1024;limits.JobMemoryLimit=limits.ProcessMemoryLimit;
    if (!job.value || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) || !AssignProcessToJobObject(job.value,process.value))
    {TerminateProcess(process.value,1);WaitForSingleObject(process.value,2000);throw std::runtime_error("Cannot enforce helper resource isolation");}
    if (ResumeThread(thread.value)==static_cast<DWORD>(-1)) {TerminateJobObject(job.value,1);WaitForSingleObject(process.value,2000);throw std::runtime_error("Cannot resume piano helper");}
    try
    {
        while (WaitForSingleObject(process.value,50)==WAIT_TIMEOUT)
        {
            if (cancel.load() || Clock::now()-start>std::chrono::seconds(seconds)) throw std::runtime_error("Piano job cancelled or timed out");
            if (log.getSize()>2*1024*1024 || log.getParentDirectory().getBytesFreeOnVolume()<3LL*1024*1024*1024) throw std::runtime_error("Piano job disk reserve reached");
            if (poll) poll();
        }
        DWORD code=1;if (!GetExitCodeProcess(process.value,&code) || code!=0) throw std::runtime_error("Piano helper failed; diagnostics retained with this job");
    }
    catch (...) {TerminateJobObject(job.value,1);WaitForSingleObject(process.value,2000);throw;}
#else
    juce::ignoreUnused(arguments,start,poll);throw std::runtime_error("Native piano helpers are unsupported on this platform");
#endif
    if (cancel.load()) throw std::runtime_error("Piano job cancelled");
}
}
