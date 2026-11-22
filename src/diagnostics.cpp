#include "diagnostics.h"
#include <windows.h>
#include <dbghelp.h>
#include <exception>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <typeinfo>
#include <corecrt.h>

namespace fatfish::diagnostics {
namespace {
HANDLE log_file=INVALID_HANDLE_VALUE;
wchar_t dump_path[32768]{};
LONG crashing=0;
void write_line(const char* data,DWORD length) noexcept {
    if (log_file!=INVALID_HANDLE_VALUE) {
        DWORD written{};
        WriteFile(log_file,data,length,&written,nullptr);
        FlushFileBuffers(log_file);
    }
    OutputDebugStringA(data);
}
void fatal(EXCEPTION_POINTERS* exception) noexcept {
    if (InterlockedCompareExchange(&crashing,1,0)!=0) return;
    char line[512]{};
    SYSTEMTIME time{}; GetLocalTime(&time);
    const auto size=std::snprintf(line,sizeof(line),
        "\r\nFATAL %04u-%02u-%02u %02u:%02u:%02u pid=%lu tid=%lu exception=0x%08lX address=%p\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
        GetCurrentProcessId(),GetCurrentThreadId(),
        exception?exception->ExceptionRecord->ExceptionCode:0,
        exception?exception->ExceptionRecord->ExceptionAddress:nullptr);
    write_line(line,static_cast<DWORD>(size));
    HANDLE dump=CreateFileW(dump_path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (dump!=INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception,FALSE};
        const BOOL saved=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),dump,
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal|MiniDumpWithThreadInfo),exception?&info:nullptr,nullptr,nullptr);
        const DWORD error=saved?0:GetLastError(); CloseHandle(dump);
        const auto count=std::snprintf(line,sizeof(line),"minidump saved=%d win32_error=%lu\r\n",saved,error);
        write_line(line,static_cast<DWORD>(count));
    } else {
        const auto count=std::snprintf(line,sizeof(line),"minidump open failed win32_error=%lu\r\n",GetLastError());
        write_line(line,static_cast<DWORD>(count));
    }
}
LONG WINAPI unhandled(EXCEPTION_POINTERS* exception) noexcept {
    fatal(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}
void terminated() noexcept {
    caught(); fatal(nullptr); TerminateProcess(GetCurrentProcess(),3); std::_Exit(3);
}
void invalid_parameter(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t) noexcept {
    record("fatal","CRT invalid parameter"); terminated();
}
void aborted(int) { record("fatal","SIGABRT"); terminated(); }
}
void initialize() noexcept {
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,32768);
    if (length && length<32768) {
        wchar_t* slash=wcsrchr(path,L'\\');
        if (slash) {
            slash[1]=0;
            wchar_t directory[32768]{};
            if (swprintf_s(directory,L"%slogs",path)>0) {
                CreateDirectoryW(directory,nullptr);
                SYSTEMTIME time{}; GetLocalTime(&time);
                wchar_t file[32768]{};
                swprintf_s(file,L"%s\\FatFishBox-%04u%02u%02u-%02u%02u%02u-%lu.log",directory,
                    time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
                swprintf_s(dump_path,L"%s.dmp",file);
                log_file=CreateFileW(file,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                if (log_file==INVALID_HANDLE_VALUE)
                    OutputDebugStringW(L"FatFishBox: cannot create diagnostic log beside executable.\n");
            }
        }
    }
    SetUnhandledExceptionFilter(unhandled);
    std::set_terminate(terminated);
    _set_invalid_parameter_handler(invalid_parameter);
    std::signal(SIGABRT,aborted);
    ULONG guarantee=32768; SetThreadStackGuarantee(&guarantee);
    record("startup","FatFishBox diagnostic handlers installed");
}
void record(std::string_view category,std::string_view message,std::source_location source) noexcept {
    const DWORD previous_error=GetLastError();
    char line[16384]{};
    SYSTEMTIME time{}; GetLocalTime(&time);
    int count=std::snprintf(line,sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu [%.*s] %s:%u %s win32_error=%lu\r\n%.*s\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,
        GetCurrentProcessId(),GetCurrentThreadId(),static_cast<int>(category.size()),category.data(),
        source.file_name(),source.line(),source.function_name(),previous_error,
        static_cast<int>((message.size()>10000)?10000:message.size()),message.data());
    DWORD used=static_cast<DWORD>(count<0?0:(count>=sizeof(line)?sizeof(line)-1:count));
    void* frames[24]{};
    const USHORT depth=CaptureStackBackTrace(1,24,frames,nullptr);
    for (USHORT i=0;i<depth && used+256<sizeof(line);++i) {
        HMODULE module{}; wchar_t module_path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]),&module);
        if (module) GetModuleFileNameW(module,module_path,MAX_PATH);
        char name[512]{};
        WideCharToMultiByte(CP_UTF8,0,module_path,-1,name,sizeof(name),nullptr,nullptr);
        const int added=std::snprintf(line+used,sizeof(line)-used,"  #%u %p %s + 0x%llX\r\n",i,frames[i],name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(module)));
        if (added<=0 || static_cast<size_t>(added)>=sizeof(line)-used) break;
        used+=added;
    }
    write_line(line,used); SetLastError(previous_error);
}
void caught(std::source_location source) noexcept {
    const auto exception=std::current_exception();
    if (!exception) { record("exception","No active C++ exception",source); return; }
    try { std::rethrow_exception(exception); }
    catch (const std::exception& error) { record(typeid(error).name(),error.what(),source); }
    catch (...) { record("exception","Unknown non-standard C++ exception",source); }
}
}
