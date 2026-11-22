#include "dsh.h"
#include "state_file.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace fatfish {
namespace {
using Process = std::unique_ptr<void,decltype(&CloseHandle)>;
constexpr auto retry_interval=35000;

fs::path recovery_file(const fs::path& auth) {
    return auth.parent_path()/L"fatfishbox-desktop-recovery.json";
}

Json read_recovery(const fs::path& file) {
    return fs::exists(file)?read_json(file):Json::object();
}

bool knows(const Json& history,const char* key,std::string_view value) {
    const auto list=history.find(key);
    return list!=history.end() && list->is_array() &&
        std::ranges::any_of(*list,[&](const Json& item){return item.is_string() && item.get_ref<const std::string&>()==value;});
}

bool process_image(HANDLE process,const fs::path& expected) {
    std::wstring image(32768,L'\0');
    auto size=static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process,0,image.data(),&size))
        throw std::runtime_error("无法核对 DSH 桌面进程");
    image.resize(size);
    return _wcsicmp(image.c_str(),expected.c_str())==0;
}

bool host_alive(DWORD pid,const fs::path& executable) {
    const auto handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!handle) {
        if (GetLastError()==ERROR_INVALID_PARAMETER) return false;
        throw std::runtime_error("无法检查 DSH 桌面 Host 是否已退出");
    }
    const Process process(handle,&CloseHandle);
    return WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT && process_image(process.get(),executable);
}

struct FailedDesktop {
    std::vector<HWND> windows;
};

BOOL CALLBACK find_failed_desktop(HWND window,LPARAM context) {
    std::array<wchar_t,128> title{};
    const auto length=GetWindowTextW(window,title.data(),static_cast<int>(title.size()));
    const std::wstring_view text(title.data(),length);
    if (text==L"DeepSeek Harness 无法使用" || text==L"DeepSeek Harness is unavailable" ||
        text==L"DeepSeek Harness Unavailable")
        reinterpret_cast<FailedDesktop*>(context)->windows.push_back(window);
    return TRUE;
}

void close_failed_desktop(const fs::path& executable) {
    FailedDesktop failed;
    EnumWindows(find_failed_desktop,reinterpret_cast<LPARAM>(&failed));
    for (const auto window:failed.windows) {
        DWORD pid{};
        GetWindowThreadProcessId(window,&pid);
        if (!pid) continue;
        const Process process(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid),&CloseHandle);
        if (!process || WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT || !process_image(process.get(),executable)) continue;
        if (!PostMessageW(window,WM_CLOSE,0,0) || WaitForSingleObject(process.get(),3500)!=WAIT_OBJECT_0)
            throw std::runtime_error("DSH Host 已退出，但桌面故障窗口未能关闭");
    }
}

void launch_desktop(const fs::path& executable) {
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(),nullptr,nullptr,nullptr,FALSE,0,nullptr,
                        executable.parent_path().c_str(),&startup,&child))
        throw std::runtime_error("无法重新启动 DSH 桌面版，Windows 错误 "+std::to_string(GetLastError()));
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
}

std::optional<Dsh> desktop_connection(const fs::path& auth) {
    if (!fs::exists(auth)) return {};
    Dsh connection(auth,1200);
    if (!connection.desktop_pid()) return {};
    return connection;
}
}

void Dsh::load_desktop_history() {
    try { desktop_history_=read_recovery(recovery_file(auth_path_)); }
    catch (const std::exception&) {}
}

bool Dsh::matches_owner(std::string_view owner) const {
    return owner==owner_ || knows(desktop_history_,"owners",owner);
}

bool Dsh::matches_desktop_origin(std::string_view origin) const {
    return origin==origin_utf8_ || knows(desktop_history_,"origins",origin);
}

bool recover_desktop_dsh(const Json& settings,std::stop_token stop,bool wait_for_restart) {
    const auto config_path=path_from_utf8(settings.value("configPath",""));
    const auto auth=path_from_utf8(settings.value("authPath",""));
    if (!config_path.is_absolute() || !auth.is_absolute() || !fs::is_regular_file(config_path)) return true;
    if (stop.stop_requested()) return false;
    const auto config=read_json(config_path);
    const auto attachment=config.value("service",Json::object()).value("attachAuthFile",Json());
    const auto command=config.value("runtime",Json::object()).value("command",Json::array());
    if (!attachment.is_string() || !command.is_array() || command.empty() || !command.front().is_string() ||
        _wcsicmp(config_relative_path(config_path,attachment.get<std::string>()).c_str(),auth.c_str())!=0) return true;
    const auto executable=config_relative_path(config_path,command.front().get<std::string>());
    if (_wcsicmp(executable.filename().c_str(),L"DeepSeek Harness.exe")!=0 || !fs::is_regular_file(executable)) return true;

    const auto file=recovery_file(auth);
    auto connection=desktop_connection(auth);
    auto recovery=read_recovery(file);
    if (!connection && (fs::exists(auth) || !recovery.value("attemptAt",std::int64_t(0)))) return true;
    if (connection && !recovery.value("attemptAt",std::int64_t(0)) &&
        knows(recovery,"owners",connection->owner()) && host_alive(connection->desktop_pid(),executable)) return true;

    {
        const ExclusiveStateLock lock(auth.parent_path()/L"fatfishbox-desktop-recovery.lock");
        connection=desktop_connection(auth);
        recovery=read_recovery(file);
        if (!connection && (fs::exists(auth) || !recovery.value("attemptAt",std::int64_t(0)))) return true;
        if (connection && !knows(recovery,"owners",connection->owner())) {
            recovery["owners"].push_back(connection->owner());
            if (!knows(recovery,"origins",connection->origin())) recovery["origins"].push_back(connection->origin());
            write_state_file(file,recovery);
        }
        if (connection && host_alive(connection->desktop_pid(),executable)) {
            if (recovery.erase("attemptAt")) write_state_file(file,recovery);
            return true;
        }
        if (stop.stop_requested()) return false;
        if (now()-recovery.value("attemptAt",std::int64_t(0))>=retry_interval) {
            recovery["attemptAt"]=now();
            write_state_file(file,recovery);
            close_failed_desktop(executable);
            launch_desktop(executable);
        }
    }

    if (!wait_for_restart) return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
    while (!stop.stop_requested() && std::chrono::steady_clock::now()<deadline) {
        try {
            auto restarted=desktop_connection(auth);
            if (restarted && (!connection || restarted->owner()!=connection->owner()) &&
                host_alive(restarted->desktop_pid(),executable)) {
                restarted->request("session/modelCatalog");
                return true;
            }
        } catch (const std::exception&) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!stop.stop_requested()) throw std::runtime_error("DSH 桌面 Host 已退出，自动重启后尚未取得可用连接，请稍后重试");
    return false;
}
}
