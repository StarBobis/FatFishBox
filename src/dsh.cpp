#include "dsh.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <memory>
#include <limits>
#include <stdexcept>
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
    result.config = value.empty() ? fs::path(env(L"DSH_WORKERS_CONFIG")) : path_from_utf8(value);
    if (!result.config.empty()) {
        auto config = read_json(result.config);
        auto auth = text(config, "/service/attachAuthFile"_json_pointer);
        if (!auth.empty()) result.auth = config_relative_path(result.config,auth);
        result.permission = text(config, "/permissions/preset"_json_pointer);
        result.preset = text(config, "/profile/agentPreset"_json_pointer);
    }
    auto auth = input.value("authPath", input.contains("configPath") ? "" : settings.value("authPath", ""));
    if (!auth.empty()) result.auth = path_from_utf8(auth);
    return result;
}

fs::path config_relative_path(const fs::path& config,std::string_view value) {
    if (value.empty()) return {};
    if (value.starts_with("~/") || value.starts_with("~\\"))
        return (fs::path(env(L"USERPROFILE")) / path_from_utf8(value.substr(2))).lexically_normal();
    return (config.parent_path() / path_from_utf8(value)).lexically_normal();
}

Dsh::Dsh(const fs::path& auth_path,int timeout_ms):timeout_ms_(timeout_ms) {
    const auto auth = read_json(auth_path);
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
}

Json Dsh::request(std::string_view method, const Json& request) const {
    return call(method, request.is_null() ? Json::object() : Json{{"request", request}});
}

Json Dsh::call(std::string_view method, const Json& args) const {
    Internet session(WinHttpOpen(L"FatFishBox/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    const auto connect_timeout=std::min(4000,timeout_ms_);
    verify(WinHttpSetTimeouts(session.handle,connect_timeout,connect_timeout,timeout_ms_,timeout_ms_), "无法设置 DSH 超时");
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
    verify(WinHttpSendRequest(request.handle, headers.c_str(), ULONG(-1), payload.data(), size, size, 0),
           "向 DSH 发送请求失败，请检查连接");
    verify(WinHttpReceiveResponse(request.handle, nullptr), "DSH 请求未返回");
    DWORD status = 0, status_length = sizeof(status);
    verify(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_length, WINHTTP_NO_HEADER_INDEX), "DSH 响应没有 HTTP 状态");
    if (status != 200) throw std::runtime_error(std::string(method) + " HTTP " + std::to_string(status));
    std::string body;
    std::vector<char> buffer(32 * 1024);
    for (;;) {
        DWORD read = 0;
        verify(WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read), "DSH 响应读取失败");
        if (!read) break;
        body.append(buffer.data(), read);
        if (body.size() > 4 * 1024 * 1024) throw std::length_error("DSH 响应超过大小上限");
    }
    const auto envelope = Json::parse(body);
    const auto& result = envelope.at("result");
    if (!result.value("ok", false)) {
        const auto error=result.find("error");
        if (error!=result.end() && error->is_object() && error->value("code","")=="response_too_large")
            throw std::length_error("DSH 响应超过大小上限");
        throw std::runtime_error("DSH 拒绝了 " + std::string(method));
    }
    return result.value("value", Json(nullptr));
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
            const auto reason=event->at("data").at("reason").value("kind","");
            std::string status;
            if (reason=="completed") status="succeeded";
            else if (reason=="aborted" || reason=="cancelled" || reason=="canceled") status="cancelled";
            else if (reason=="interrupted" || reason=="forked") status="interrupted";
            else if (reason=="blocked" || reason=="error" || reason=="max-tokens") status="failed";
            else return {};
            return TurnEnd{std::move(status),seq,event->value("time",std::int64_t(0))};
        }
        if (events.empty() || !page.value("hasMore",false)) break;
        args["beforeSeq"]=events.back()->at("seq");
    }
    return {};
}
}
