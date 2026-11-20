#pragma once

#include "cluster.h"
#include <windows.h>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fatfish {
class ExclusiveStateLock {
public:
    explicit ExclusiveStateLock(const fs::path& path) {
        handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle_==INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开交接状态锁");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!LockFileEx(handle_,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&range_)) {
            if (GetLastError()!=ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now()>=deadline) {
                CloseHandle(handle_);
                throw std::runtime_error("交接状态锁繁忙，请重试");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ExclusiveStateLock(const ExclusiveStateLock&)=delete;
    ~ExclusiveStateLock() { UnlockFileEx(handle_,0,1,0,&range_); CloseHandle(handle_); }
private:
    HANDLE handle_{};
    OVERLAPPED range_{};
};

inline void write_state_file(const fs::path& target,const Json& value) {
    const auto temp=target.parent_path()/path_from_utf8("handoff-"+id()+".tmp");
    try {
        std::ofstream file(temp,std::ios::binary);
        const auto data=value.dump();
        file.write(data.data(),static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file || !MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("无法保存交接状态");
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp,ignored);
        throw;
    }
}
}
