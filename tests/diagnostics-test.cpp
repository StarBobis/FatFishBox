#include "diagnostics.h"
#include <windows.h>
#include <stdexcept>
#include <thread>
#include <string_view>
int main(int argc,char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    fatfish::diagnostics::initialize();
    const std::string_view mode=argc>1?argv[1]:"recover";
    if (mode=="seh") { RaiseException(0xE0000123,EXCEPTION_NONCONTINUABLE,0,nullptr); return 9; }
    if (mode=="terminate") { std::thread worker([] { throw std::runtime_error("worker fatal probe"); }); worker.join(); return 9; }
    try { throw std::runtime_error("可恢复异常 probe"); }
    catch (...) { fatfish::diagnostics::caught(); }
    std::thread worker([] {
        try { throw 42; } catch (...) { fatfish::diagnostics::caught(); }
    }); worker.join();
    fatfish::diagnostics::record("continued","process remains alive after recoverable exceptions");
    return 0;
}
