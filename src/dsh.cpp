#include "dsh.h"
#include <cctype>
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <fstream>
#include <memory>
#include <limits>
#include <regex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fatfish {
namespace {
struct Internet {
    HINTERNET handle = nullptr;
    explicit Internet(HINTERNET h) : handle(h) {
        if (!handle) throw std::runtime_error("无法连接 DSH 本机服务");
    }
    Internet(const Internet&) = delete;
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
};
void verify(BOOL result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
std::wstring env(const wchar_t* name) {
    const auto count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    std::wstring value(count, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), count));
    return value;
}
std::string text(const Json& value, const Json::json_pointer& path) {
    if (!value.contains(path) || !value.at(path).is_string()) return {};
    return value.at(path).get<std::string>();
}
bool existing_file(const fs::path& path) {
    std::error_code error;
    return path.is_absolute() && fs::is_regular_file(path,error);
}
fs::path dsh_home() {
    auto home=fs::path(env(L"DSH_HOME"));
    if (home.empty()) home=fs::path(env(L"USERPROFILE"))/L".dsh";
    return home;
}
fs::path desktop_auth_path() {
    return fs::path(env(L"LOCALAPPDATA"))/L"FatFishBox"/L"dsh-desktop"/L"auth-private.json";
}
fs::path desktop_cli(const fs::path& executable) {
    return executable.parent_path()/LR"(resources\app.asar\dsh\node_modules\@deepseek-ai\dsh-desktop-host\lib\cli.js)";
}
bool desktop_installation(const fs::path& executable) {
    return _wcsicmp(executable.filename().c_str(),L"DeepSeek Harness.exe")==0 &&
        existing_file(executable) &&
        existing_file(executable.parent_path()/LR"(resources\runtime\cli\bin\dsh.cmd)") &&
        existing_file(executable.parent_path()/LR"(resources\app.asar)");
}
fs::path find_desktop_executable() {
    wchar_t command[32768]{};
    DWORD bytes=sizeof(command);
    if (RegGetValueW(HKEY_CURRENT_USER,LR"(Software\Classes\dsh\shell\open\command)",nullptr,
                     RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,command,&bytes)==ERROR_SUCCESS) {
        int count=0;
        const std::unique_ptr<wchar_t*,decltype(&LocalFree)> args(CommandLineToArgvW(command,&count),LocalFree);
        if (args && count && desktop_installation(args.get()[0])) return args.get()[0];
    }
    for (const auto& dir:{fs::path(env(L"LOCALAPPDATA"))/L"Programs",fs::path(env(L"ProgramFiles"))}) {
        const auto executable=dir/L"DeepSeek Harness"/L"DeepSeek Harness.exe";
        if (desktop_installation(executable)) return executable;
    }
    throw DshError("找不到 DSH 的桌面命令。可改用 DSH「插件」页安装连接插件。","desktop_cli_missing");
}
bool desktop_window_exists(const fs::path& executable) {
    struct Search { const fs::path& executable; bool found=false; } search{executable};
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        auto& search=*reinterpret_cast<Search*>(data);
        wchar_t window_class[64]{};
        GetClassNameW(window,window_class,static_cast<int>(std::size(window_class)));
        if (!std::wstring_view(window_class).starts_with(L"Chrome_WidgetWin_")) return TRUE;
        DWORD pid{};
        GetWindowThreadProcessId(window,&pid);
        const std::unique_ptr<void,decltype(&CloseHandle)> process(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid),CloseHandle);
        if (!process) return TRUE;
        std::wstring image(32768,L'\0');
        auto size=static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process.get(),0,image.data(),&size)) return TRUE;
        image.resize(size);
        search.found=_wcsicmp(image.c_str(),search.executable.c_str())==0;
        return !search.found;
    },reinterpret_cast<LPARAM>(&search));
    return search.found;
}
void write_plugin_resource(const fs::path& target,int resource_id) {
    const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(resource_id),RT_RCDATA);
    const auto loaded=resource?LoadResource(nullptr,resource):nullptr;
    const auto* data=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
    const auto size=resource?SizeofResource(nullptr,resource):0;
    if (!data || !size) throw std::runtime_error("无法读取 EXE 内置的 DSH 接入插件");
    const auto temporary=target.parent_path()/path_from_utf8("plugin-"+id()+".tmp");
    try {
        std::ofstream file(temporary,std::ios::binary);
        file.write(data,size);
        file.close();
        if (!file || !MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("无法写入本机 DSH 接入插件");
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary,ignored);
        throw;
    }
}
std::vector<wchar_t> desktop_cli_environment() {
    const std::unique_ptr<wchar_t,decltype(&FreeEnvironmentStringsW)> entries(GetEnvironmentStringsW(),FreeEnvironmentStringsW);
    if (!entries) throw std::runtime_error("无法读取 DSH 命令所需的环境变量");
    std::vector<std::wstring> variables{L"ELECTRON_RUN_AS_NODE=1"};
    for (auto value=entries.get();*value;value+=wcslen(value)+1) {
        if (_wcsnicmp(value,L"ELECTRON_RUN_AS_NODE=",21)!=0) variables.emplace_back(value);
    }
    std::ranges::sort(variables,[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> block;
    for (const auto& value:variables) block.insert(block.end(),value.c_str(),value.c_str()+value.size()+1);
    block.push_back(L'\0');
    return block;
}
std::string desktop_version(DWORD pid) {
    const std::unique_ptr<void,decltype(&CloseHandle)> process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid),CloseHandle);
    if (!process) return {};
    std::wstring path(32768,L'\0');
    auto size=static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(),0,path.data(),&size)) return {};
    path.resize(size);
    if (_wcsicmp(fs::path(path).filename().c_str(),L"DeepSeek Harness.exe")!=0) return {};
    DWORD unused=0;
    const auto info_size=GetFileVersionInfoSizeW(path.c_str(),&unused);
    if (!info_size || info_size>1024*1024) return {};
    std::vector<char> info(info_size);
    if (!GetFileVersionInfoW(path.c_str(),0,info_size,info.data())) return {};
    struct Translation { WORD language,code_page; };
    void* translations=nullptr;
    UINT bytes=0;
    if (!VerQueryValueW(info.data(),L"\\VarFileInfo\\Translation",&translations,&bytes)) return {};
    const auto* codes=static_cast<const Translation*>(translations);
    for (std::size_t i=0;i<bytes/sizeof(Translation);++i) {
        wchar_t key[64]{};
        swprintf_s(key,L"\\StringFileInfo\\%04x%04x\\FileVersion",
            static_cast<unsigned>(codes[i].language),static_cast<unsigned>(codes[i].code_page));
        void* value=nullptr;
        UINT length=0;
        if (VerQueryValueW(info.data(),key,&value,&length) && length>1 && length<=201)
            return narrow(std::wstring_view(static_cast<const wchar_t*>(value),length-1));
    }
    return {};
}
fs::path default_state() {
    auto state=fs::path(env(L"DSH_WORKERS_STATE"));
    if (state.empty()) state=fs::path(env(L"LOCALAPPDATA"))/L"dsh-workers";
    return state;
}
fs::path current_service_auth(const fs::path& state) {
    const auto services=state/L"services";
    if (!existing_file(services/L"current.json")) return {};
    const auto marker=read_json(services/L"current.json");
    const auto service=text(marker,"/serviceId"_json_pointer);
    const auto alpha_numeric=[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9');
    };
    if (service.empty() || service.size()>200 || !alpha_numeric(service.front()) ||
        !std::ranges::all_of(service,[&](unsigned char c){return alpha_numeric(c) || c=='-' || c=='_' || c=='.';}))
        throw std::runtime_error("DSH 当前服务标记无效");
    return services/path_from_utf8(service)/L"auth-private.json";
}
fs::path managed_state(const fs::path& auth) {
    const auto services=auth.parent_path().parent_path();
    if (auth.is_absolute() && _wcsicmp(auth.filename().c_str(),L"auth-private.json")==0 &&
        _wcsicmp(services.filename().c_str(),L"services")==0) return services.parent_path();
    return {};
}
void transport_error(DWORD code,const char* message) {
    if (code==ERROR_WINHTTP_CANNOT_CONNECT || code==ERROR_WINHTTP_NAME_NOT_RESOLVED)
        message="无法连接 DSH 本机服务，请确认 DSH 正在运行且本机授权文件属于当前实例";
    else if (code==ERROR_WINHTTP_TIMEOUT)
        message="DSH 请求超时，请确认服务已启动并检查模型提供方的连接";
    else if (code==ERROR_WINHTTP_SECURE_FAILURE)
        message="无法验证 DSH 本机服务的 HTTPS 证书";
    throw DshError(std::string(message)+"（Windows "+std::to_string(code)+"）","winhttp_"+std::to_string(code));
}
}

Json read_json(const fs::path& path, std::size_t limit) {
    if (!path.is_absolute()) throw std::runtime_error("文件路径必须是绝对路径");
    const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("无法读取本地文件");
    const std::unique_ptr<void,decltype(&CloseHandle)> file(handle,CloseHandle);
    LARGE_INTEGER size{};
    if (GetFileType(file.get())!=FILE_TYPE_DISK || !GetFileSizeEx(file.get(),&size) ||
        size.QuadPart<0 || static_cast<std::uint64_t>(size.QuadPart)>limit)
        throw std::runtime_error("本地文件无效或超过大小限制");
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');
    for (std::size_t offset=0;offset<bytes.size();) {
        DWORD read=0;
        const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024*1024));
        if (!ReadFile(file.get(),bytes.data()+offset,count,&read,nullptr) || !read)
            throw std::runtime_error("读取期间本地文件发生变化");
        offset+=read;
    }
    return Json::parse(bytes);
}

DshConnection discover(const Json& settings, const Json& input) {
    DshConnection result;
    const auto value = input.value("configPath", settings.value("configPath", ""));
    const bool automatic=input.value("automaticAuth",
        !input.contains("authPath") && !input.contains("configPath") && settings.value("automaticAuth",false));
    const auto auth = input.value("authPath", input.contains("configPath") ? "" : settings.value("authPath", ""));
    result.config = path_from_utf8(value);
    if (automatic && result.config.empty() && !auth.empty()) {
        if (const auto state=managed_state(path_from_utf8(auth));!state.empty()) {
            result.auth=current_service_auth(state);
            return result;
        }
    }
    if (result.config.empty() && auth.empty()) {
        result.config = fs::path(env(L"DSH_WORKERS_CONFIG"));
        if (result.config.empty()) {
            const auto standard=fs::path(env(L"USERPROFILE"))/L".deepseek-workers"/L"workers.json";
            if (existing_file(standard)) result.config=standard;
        }
    }
    fs::path state;
    if (!result.config.empty()) {
        Json config;
        try { config=read_json(result.config); }
        catch (const std::exception&) { throw std::runtime_error("无法读取 DSH 配置文件，请检查路径和 JSON 格式"); }
        result.auth = config_relative_path(result.config,text(config, "/service/attachAuthFile"_json_pointer));
        state = config_relative_path(result.config,text(config, "/stateDir"_json_pointer));
        result.permission = text(config, "/permissions/preset"_json_pointer);
        result.preset = text(config, "/profile/agentPreset"_json_pointer);
    }
    if (!auth.empty() && !automatic) result.auth = path_from_utf8(auth);
    if (result.auth.empty()) {
        if (state.empty()) state=default_state();
        result.auth=current_service_auth(state);
        if (result.auth.empty()) {
            const auto standard=state/L"desktop-attach"/L"auth-private.json";
            if (existing_file(standard)) result.auth=standard;
        }
    }
    return result;
}

fs::path extract_dsh_desktop_plugin() {
    const auto plugin=desktop_auth_path().parent_path()/L"plugin";
    if (!plugin.is_absolute()) throw std::runtime_error("无法定位本机插件目录");
    fs::create_directories(plugin);
    write_plugin_resource(plugin/L"index.cjs",IDR_DSH_PLUGIN_SCRIPT);
    write_plugin_resource(plugin/L"cordis.patch.yml",IDR_DSH_PLUGIN_PATCH);
    write_plugin_resource(plugin/L"package.json",IDR_DSH_PLUGIN_MANIFEST);
    return plugin;
}

void authorize_dsh_desktop() {
    const auto manifest=dsh_home()/LR"(profiles\desktop\package.json)";
    if (!existing_file(manifest)) throw std::runtime_error("请先打开 DSH 桌面版完成初始化，再完全退出");
    const auto executable=find_desktop_executable();
    const auto wait_for_exit=GetTickCount64()+45000;
    while (desktop_window_exists(executable)) {
        if (GetTickCount64()>=wait_for_exit)
            throw std::runtime_error("DSH 仍在运行。请从 DSH「应用」菜单退出，再点「自动授权」");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    const auto plugin=extract_dsh_desktop_plugin();
    auto command=L"\""+executable.native()+L"\" --expose-internals \""+desktop_cli(executable).native()+
        L"\" plugin --profile desktop add \""+plugin.native()+L"\"";
    auto environment=desktop_cli_environment();
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,
                        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),plugin.parent_path().c_str(),&startup,&child))
        throw std::runtime_error("无法启动 DSH 桌面命令，Windows 错误 "+std::to_string(GetLastError()));
    CloseHandle(child.hThread);
    const std::unique_ptr<void,decltype(&CloseHandle)> process(child.hProcess,CloseHandle);
    DWORD code=0;
    if (WaitForSingleObject(process.get(),180000)!=WAIT_OBJECT_0)
        throw std::runtime_error("DSH 插件安装未在预期时间完成，请在 DSH 的「插件」页检查");
    if (!GetExitCodeProcess(process.get(),&code) || code!=0)
        throw std::runtime_error("DSH 命令未能安装插件，请使用 DSH「插件」页安装");
    const auto profile=read_json(manifest);
    const auto bundles="/dsh/profile/bundles"_json_pointer;
    if (!profile.contains(bundles) || std::ranges::find(profile.at(bundles),"fatfishbox-dsh-desktop")==profile.at(bundles).end())
        throw std::runtime_error("插件尚未启用，请在 DSH 的「插件」页启用");
    if (!CreateProcessW(executable.c_str(),nullptr,nullptr,nullptr,FALSE,0,nullptr,
                        executable.parent_path().c_str(),&startup,&child))
        throw std::runtime_error("插件已安装，请打开 DSH 桌面版后重新自动识别");
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    const auto deadline=GetTickCount64()+25000;
    do {
        if (existing_file(desktop_auth_path())) {
            try { Dsh(desktop_auth_path(),1000,2000).probe(); return; }
            catch (const std::exception&) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    } while (GetTickCount64()<deadline);
    throw std::runtime_error("插件已安装，DSH 尚未发布可用授权。请等 DSH 启动完成后重新自动识别");
}

Json discover_dsh_connections(const Json& hints) {
    struct Candidate { Json row; fs::path config,auth; };
    std::vector<Candidate> candidates;
    const auto add=[&](const fs::path& config,const fs::path& auth,bool automatic) {
        if (candidates.size()>=64 || (!config.is_absolute() && !auth.is_absolute())) return;
        const auto same=[](const fs::path& a,const fs::path& b) {
            return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;
        };
        if (std::ranges::any_of(candidates,[&](const Candidate& row){return same(config,row.config) && same(auth,row.auth);})) return;
        Json row{{"configPath",utf8(config)},{"authPath",utf8(auth)},{"automaticAuth",automatic}};
        fs::path resolved=auth;
        try {
            const auto connection=discover(Json::object(),row);
            resolved=connection.auth;
            row.update({{"authPath",utf8(resolved)},{"permission",connection.permission},{"preset",connection.preset}});
        } catch (const std::exception&) {
            row["error"]="无法读取 DSH 配置或当前服务标记";
        }
        row["name"]=config.empty()?utf8(auth.parent_path().filename()):utf8(config.stem());
        candidates.push_back({std::move(row),config,auth});
    };
    const auto home_dir=dsh_home();
    const auto desktop_auth=desktop_auth_path();
    if (desktop_auth.is_absolute() &&
        (existing_file(desktop_auth) || existing_file(home_dir/L"profiles"/L"desktop"/L"package.json"))) {
        add({},desktop_auth,false);
        candidates.back().row.update({
            {"name","DSH 桌面版"},{"desktopPlugin",true},
        });
    }
    const auto env_config=fs::path(env(L"DSH_WORKERS_CONFIG"));
    add(env_config,{},true);
    for (const auto& hint:hints) {
        const auto config=path_from_utf8(hint.value("configPath",""));
        const auto auth=path_from_utf8(hint.value("authPath",""));
        if (!config.empty()) add(config,{},true);
        if (!auth.empty() && (config.empty() || !hint.value("automaticAuth",false))) {
            const auto resolved=std::ranges::find_if(candidates,[&](const Candidate& candidate) {
                return _wcsicmp(candidate.config.c_str(),config.c_str())==0 &&
                    _wcsicmp(widen(candidate.row.value("authPath","")).c_str(),auth.c_str())==0;
            });
            if (resolved==candidates.end()) add(config,auth,hint.value("automaticAuth",false));
        }
    }
    const auto home=fs::path(env(L"USERPROFILE"))/L".deepseek-workers";
    if (home.is_absolute()) {
        std::error_code error;
        for (fs::directory_iterator it(home,error); !error && it!=fs::directory_iterator() && candidates.size()<64;
             it.increment(error)) {
            const auto& path=it->path();
            if (_wcsicmp(path.extension().c_str(),L".json")!=0 || !existing_file(path)) continue;
            try {
                const auto value=read_json(path);
                if (value.contains("pools") || value.contains("stateDir") || value.contains("/service/attachAuthFile"_json_pointer))
                    add(path,{},true);
            } catch (const std::exception&) {}
        }
    }
    const auto inspect_state=[&](const fs::path& state) {
        const auto add_auth=[&](const fs::path& auth,bool automatic) {
            if (!auth.empty() && std::ranges::none_of(candidates,[&](const Candidate& row){
                const auto resolved=path_from_utf8(row.row.value("authPath","")).lexically_normal();
                return _wcsicmp(resolved.c_str(),auth.lexically_normal().c_str())==0;
            })) add({},auth,automatic);
        };
        const auto desktop=state/L"desktop-attach"/L"auth-private.json";
        if (existing_file(desktop)) add_auth(desktop,false);
        try { add_auth(current_service_auth(state),true); } catch (const std::exception&) {}
    };
    inspect_state(default_state());
    inspect_state(fs::path(env(L"LOCALAPPDATA"))/L"dsh-workers");
    for (const auto& root:{home_dir,home}) {
        if (!root.is_absolute()) continue;
        std::error_code error;
        for (fs::directory_iterator it(root,error); !error && it!=fs::directory_iterator() && candidates.size()<64;
             it.increment(error)) {
            if (it->is_directory(error) && !error) inspect_state(it->path());
        }
    }
    std::atomic_size_t next{};
    std::vector<std::jthread> workers;
    for (std::size_t i=0;i<std::min<std::size_t>(8,candidates.size());++i) {
        workers.emplace_back([&] {
            for (auto index=next.fetch_add(1);index<candidates.size();index=next.fetch_add(1)) {
                auto& row=candidates[index].row;
                row["reachable"]=false;
                if (row.contains("error")) continue;
                const auto auth=path_from_utf8(row.at("authPath").get<std::string>());
                if (!existing_file(auth)) {
                    row["error"]="未找到本机授权文件";
                    if (row.value("desktopPlugin",false)) row["setupRequired"]=true;
                    continue;
                }
                try {
                    const Dsh dsh(auth,1500,3000);
                    dsh.probe();
                    row["reachable"]=true;
                    if (dsh.desktop_pid()) row["desktop"]=true;
                } catch (const std::exception&) {
                    row["error"]="本机服务未响应或授权无效";
                    if (row.value("desktopPlugin",false)) row["setupRequired"]=true;
                }
            }
        });
    }
    for (auto& worker:workers) worker.join();
    if (std::ranges::any_of(candidates,[](const Candidate& item){return item.row.value("desktop",false);}))
        std::erase_if(candidates,[](const Candidate& item){return item.row.value("setupRequired",false);});
    std::stable_partition(candidates.begin(),candidates.end(),[](const Candidate& item){return item.row.at("reachable")==true;});
    Json result=Json::array();
    for (auto& candidate:candidates) result.push_back(std::move(candidate.row));
    return result;
}

fs::path config_relative_path(const fs::path& config,std::string_view value) {
    if (value.empty()) return {};
    if (value.starts_with("~/") || value.starts_with("~\\"))
        return (fs::path(env(L"USERPROFILE")) / path_from_utf8(value.substr(2))).lexically_normal();
    return (config.parent_path() / path_from_utf8(value)).lexically_normal();
}

Dsh::Dsh(const fs::path& auth_path,int timeout_ms,int total_timeout_ms):auth_path_(auth_path),timeout_ms_(timeout_ms),
    deadline_(total_timeout_ms>0?GetTickCount64()+static_cast<std::uint64_t>(total_timeout_ms):0) {
    Json auth;
    try { auth=read_json(auth_path); }
    catch (const std::exception&) {
        throw std::runtime_error("无法读取本机 DSH 授权文件，请检查路径并确认 DSH 已发布授权");
    }
    if (!auth.is_object() || !auth.contains("origin") || !auth.at("origin").is_string() ||
        !auth.contains("cookie") || !auth.at("cookie").is_string())
        throw std::runtime_error("本机 DSH 授权文件缺少连接地址或凭据");
    const auto pid=auth.value("desktopHostPid",std::int64_t(0));
    if (pid>0 && pid<=std::numeric_limits<unsigned long>::max()) desktop_pid_=static_cast<unsigned long>(pid);
    recorded_version_=text(auth,"/runtimeVersion"_json_pointer);
    const auto address = widen(auth.at("origin").get<std::string>());
    URL_COMPONENTS url{};
    url.dwStructSize = sizeof(url);
    url.dwHostNameLength = url.dwSchemeLength = url.dwUserNameLength =
        url.dwPasswordLength = url.dwUrlPathLength = url.dwExtraInfoLength = DWORD(-1);
    verify(WinHttpCrackUrl(address.c_str(), 0, 0, &url), "DSH 地址格式错误");
    host_.assign(url.lpszHostName, url.dwHostNameLength);
    https_ = url.nScheme == INTERNET_SCHEME_HTTPS;
    if ((!https_ && url.nScheme != INTERNET_SCHEME_HTTP) ||
        (host_ != L"127.0.0.1" && host_ != L"localhost" && host_ != L"[::1]" && host_ != L"::1") ||
        url.dwUserNameLength || url.dwPasswordLength || url.dwExtraInfoLength ||
        (url.dwUrlPathLength && std::wstring_view(url.lpszUrlPath, url.dwUrlPathLength) != L"/"))
        throw std::runtime_error("DSH 连接必须是未经重定向的本机 HTTP 地址");
    port_ = url.nPort;
    origin_ = address;
    if (origin_.ends_with(L'/')) origin_.pop_back();
    cookie_ = widen(auth.at("cookie").get<std::string>());
    if (cookie_.empty() || cookie_.find_first_of(L"\r\n") != std::wstring::npos || cookie_.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("DSH 授权文件缺少有效的连接凭据");
    origin_utf8_=narrow(origin_);
    owner_ = origin_utf8_ + "#" + auth.value("connectionId", "");
    if (desktop_pid_) load_desktop_history();
}

Json Dsh::request(std::string_view method, const Json& request) const {
    try { return call(method, request.is_null() ? Json::object() : Json{{"request", request}}); }
    catch (const DshError& error) { throw DshError(error.what(),error.code,std::string(method)); }
}

Json dsh_error_diagnostics(const DshError& error) {
    const auto& code=error.code;
    const bool retryable=code.starts_with("winhttp_") || code=="deadline" || code=="http_408" ||
        code=="http_429" || code=="http_502" || code=="http_503" || code=="http_504" || code=="rate_limited";
    return {{"reason",code},{"stage",error.stage},{"retryable",retryable},{"retryAfterMs",retryable?2000:0}};
}

void Dsh::probe() const {
    try { request("permissionPresets/catalog"); }
    catch (const DshError& error) {
        if (error.code.starts_with("winhttp_") || error.code=="http_401" || error.code=="http_403") throw;
        request("session/modelCatalog");
    }
}

std::string Dsh::runtime_version() const {
    auto version=desktop_pid_?desktop_version(desktop_pid_):recorded_version_;
    if (version.starts_with('v')) version.erase(0,1);
    static const std::regex format(R"([0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?)");
    if (version.size()>200 || !std::regex_match(version,format))
        throw std::runtime_error("已连接 DSH，但当前服务没有提供可识别的版本");
    return version;
}

Json Dsh::call(std::string_view method, const Json& args) const {
    int timeout=timeout_ms_;
    if (deadline_) {
        const auto started=GetTickCount64();
        if (started>=deadline_) throw DshError("DSH 操作已达到等待上限","deadline");
        timeout=static_cast<int>(std::min<std::uint64_t>(timeout,deadline_-started));
    }
    Internet session(WinHttpOpen(L"FatFishBox/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    const auto connect_timeout=std::min(4000,timeout);
    verify(WinHttpSetTimeouts(session.handle,connect_timeout,connect_timeout,timeout,timeout), "无法设置 DSH 超时");
    Internet connection(WinHttpConnect(session.handle, host_.c_str(), port_, 0));
    const std::wstring path = L"/api/" + widen(method);
    Internet request(WinHttpOpenRequest(connection.handle, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https_ ? WINHTTP_FLAG_SECURE : 0));
    DWORD flags = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    verify(WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags)), "无法限制 DSH 连接");
    const auto headers = L"Content-Type: application/json\r\nAccept: application/json\r\nOrigin: " + origin_
        + L"\r\nCookie: " + cookie_ + L"\r\n";
    auto payload = Json{{"type","client-request"},{"rpcId",id()},{"method",method},{"payload",{{"args",args}}}}.dump();
    auto size = static_cast<DWORD>(payload.size());
    if (!WinHttpSendRequest(request.handle,headers.c_str(),ULONG(-1),payload.data(),size,size,0))
        transport_error(GetLastError(),"向 DSH 发送请求失败");
    if (!WinHttpReceiveResponse(request.handle,nullptr))
        transport_error(GetLastError(),"读取 DSH 响应失败");
    DWORD status = 0, status_length = sizeof(status);
    verify(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_length, WINHTTP_NO_HEADER_INDEX), "DSH 响应没有 HTTP 状态");
    if (status==401 || status==403)
        throw DshError("DSH 拒绝了本机授权，请检查授权文件是否属于当前实例（HTTP "+std::to_string(status)+"）",
            "http_"+std::to_string(status));
    if (status != 200) throw DshError(std::string(method) + " HTTP " + std::to_string(status),"http_"+std::to_string(status));
    std::string body;
    std::vector<char> buffer(32 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle,buffer.data(),static_cast<DWORD>(buffer.size()),&read))
            throw DshError("DSH 响应读取失败","winhttp_"+std::to_string(GetLastError()));
        if (!read) break;
        body.append(buffer.data(), read);
        if (body.size() > 4 * 1024 * 1024) throw std::length_error("DSH 响应超过大小上限");
    }
    const auto envelope = Json::parse(body);
    const auto& result = envelope.at("result");
    if (!result.value("ok", false)) {
        const auto error=result.find("error");
        const auto code=error!=result.end() && error->is_object()?error->value("code",error->value("type","")):result.value("code","");
        if (code=="response_too_large")
            throw std::length_error("DSH 响应超过大小上限");
        throw DshError("DSH 拒绝了 " + std::string(method),code);
    }
    return result.value("value", Json(nullptr));
}

Json failure_details(const Json& reason) {
    Json failure=Json::object();
    if (!reason.is_object()) return failure;
    const auto kind=text(reason,"/kind"_json_pointer);
    if (kind.empty() || kind=="completed" || kind=="aborted" || kind=="cancelled" || kind=="canceled") return failure;
    failure["kind"]=kind;
    const auto error=reason.find("error");
    const auto& details=error!=reason.end() && error->is_object()?*error:reason;
    const auto code=text(details,"/code"_json_pointer);
    auto message=text(details,"/message"_json_pointer);
    if (message.empty() && error!=reason.end() && error->is_string()) message=error->get<std::string>();
    if (!code.empty()) failure["code"]=code;
    if (!message.empty()) failure["message"]=message;
    auto normalized=code;
    std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
    int status=0;
    for (const auto* member:{"httpStatus","statusCode","status"}) if (details.contains(member) && details.at(member).is_number_integer())
        status=details.at(member).get<int>();
    if (normalized.starts_with("http_") && normalized.size()==8 && normalized.substr(5).find_first_not_of("0123456789")==std::string::npos)
        status=std::stoi(normalized.substr(5));
    if (details.contains("retryable") && details.at("retryable").is_boolean()) {
        failure["retryable"]=details.at("retryable");failure["retryabilitySource"]="upstream";
    } else if (status>=400 && status<=599) {
        failure["retryable"]=status==408 || status==429 || status==500 || status==502 || status==503 || status==504;
        failure["retryabilitySource"]="http_status";failure["httpStatus"]=status;
    } else if (normalized=="rate_limited" || normalized=="timeout" || normalized=="deadline") {
        failure["retryable"]=true;failure["retryabilitySource"]="error_code";
    }
    if (failure.contains("retryable")) {
        failure["retryAfterMs"]=failure.at("retryable").get<bool>()?2000:0;
        if (details.contains("retryAfterMs") && details.at("retryAfterMs").is_number_integer())
            failure["retryAfterMs"]=std::clamp<std::int64_t>(details.at("retryAfterMs").get<std::int64_t>(),0,3600000);
        failure["stage"]="execution";failure["retryMode"]="new_request";
    }
    return failure;
}

bool has_queued_prompt(const Json& state, std::string_view request_id) {
    const auto queue=state.find("queue");
    if (queue==state.end() || !queue->is_array()) return false;
    return std::ranges::any_of(*queue,[&](const Json& item) {
        if (!item.is_object()) return false;
        const auto placement=text(item,"/placement"_json_pointer);
        if (placement!="queued" && placement!="steering") return false;
        const auto rpc_id=text(item,"/rpcId"_json_pointer);
        return request_id.empty() || rpc_id.empty() || rpc_id==request_id;
    });
}

std::optional<TurnEnd> latest_turn_end(const Dsh& dsh,const Json& session_id,
                                      std::int64_t start,std::int64_t cursor) {
    if (cursor<=start) return {};
    Json args{{"address",{{"kind","session"},{"sessionId",session_id}}},{"throughSeq",cursor},{"maxMessages",4}};
    for (int page_number=0;page_number<3;++page_number) {
        Json page;
        for (;;) {
            try { page=dsh.request("session/page",args);break; }
            catch (const std::length_error&) {
                auto size=args.at("maxMessages").get<int>();
                if (size<=1) throw;
                args["maxMessages"]=size/2;
            }
        }
        const auto& records=page.at("records");
        if (!records.is_array()) throw std::runtime_error("DSH 历史记录格式无效");
        const auto before=args.value("beforeSeq",std::numeric_limits<std::int64_t>::max());
        std::vector<const Json*> events;
        for (const auto& wrapper:records) {
            const auto event=wrapper.find("event");
            if (event==wrapper.end() || !event->is_object()) continue;
            const auto seq=event->value("seq",std::int64_t(-1));
            if (seq>=0 && seq<=cursor && seq<before) events.push_back(&*event);
        }
        std::sort(events.begin(),events.end(),[](const Json* a,const Json* b){return a->at("seq")>b->at("seq");});
        for (const auto* event:events) {
            const auto seq=event->at("seq").get<std::int64_t>();
            const auto type=event->value("type","");
            if (seq<=start || type=="turn/start") return {};
            if (type!="turn/end") continue;
            const auto& details=event->at("data").at("reason");
            const auto reason=text(details,"/kind"_json_pointer);
            std::string status;
            if (reason=="completed") status="succeeded";
            else if (reason=="aborted" || reason=="cancelled" || reason=="canceled") status="cancelled";
            else if (reason=="interrupted" || reason=="forked") status="interrupted";
            else if (reason=="blocked" || reason=="error" || reason=="max-tokens") status="failed";
            else return {};
            return TurnEnd{std::move(status),seq,event->value("time",std::int64_t(0)),failure_details(details)};
        }
        if (events.empty() || !page.value("hasMore",false)) break;
        args["beforeSeq"]=events.back()->at("seq");
    }
    return {};
}
}
