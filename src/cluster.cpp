#include "cluster.h"
#include "dsh.h"
#include "legacy.h"
#include "state_file.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace fatfish {
std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("UTF-8 输入无效");
    std::wstring output(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), output.data(), count);
    return output;
}
std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Unicode 输入无效");
    std::string output(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), output.data(), count, nullptr, nullptr);
    return output;
}
std::string utf8(const fs::path& path) { return narrow(path.native()); }
std::string id() {
    GUID guid;
    if (CoCreateGuid(&guid) < 0) throw std::runtime_error("无法生成操作标识");
    wchar_t text[40]{};
    StringFromGUID2(guid, text, 40);
    return narrow(std::wstring_view(text + 1, 36));
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::wstring environment(const wchar_t* name) {
    auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return value;
}
const std::set<std::string> roles{"developer","reviewer","repair","support"};
bool active(const Json& row) {
    const auto state = row.value("status", "");
    return state != "succeeded" && state != "failed" && state != "cancelled" && state != "interrupted";
}
Json settings_template() {
    return {{"revision",0},{"configPath",""},{"authPath",""},{"permission",""},{"preset",""},
        {"balance","least_loaded"},{"accounts",Json::object()},{"pools",Json::object()}};
}
class Database {
public:
    Database(const fs::path& folder, bool writable) : folder_(folder), writable_(writable) {
        lock_ = CreateFileW((folder / L"state.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock_ == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开本地状态锁");
        if (!LockFileEx(lock_, writable ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, 1, 0, &range_)) {
            CloseHandle(lock_);
            throw std::runtime_error("无法锁定本地状态");
        }
        try {
            const auto file = folder / L"state.json";
            state = fs::exists(file) ? read_json(file, 64 * 1024 * 1024)
                : Json{{"settings",settings_template()},{"sessions",Json::object()}};
        } catch (...) {
            UnlockFileEx(lock_, 0, 1, 0, &range_);
            CloseHandle(lock_);
            throw;
        }
    }
    Database(const Database&) = delete;
    ~Database() {
        UnlockFileEx(lock_, 0, 1, 0, &range_);
        CloseHandle(lock_);
    }
    void save() const {
        require(writable_, "本地状态锁没有写权限");
        const auto temp = folder_ / path_from_utf8("state-" + id() + ".tmp");
        try {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            const auto data = state.dump();
            output.write(data.data(), std::streamsize(data.size()));
            output.close();
            require(bool(output), "本地状态写入失败");
            require(MoveFileExW(temp.c_str(), (folder_ / L"state.json").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "无法提交本地状态");
        } catch (...) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            throw;
        }
    }
    Json state;
private:
    fs::path folder_;
    bool writable_;
    HANDLE lock_{};
    OVERLAPPED range_{};
};
std::string field(const Json& source, std::string_view key, std::size_t max) {
    const auto value = source.at(key).get<std::string>();
    require(!value.empty() && value.size() <= max && value.find('\0') == std::string::npos, "缺少字段或长度无效");
    return value;
}
void validate_configuration(const Json& settings) {
    const auto balance = settings.at("balance").get<std::string>();
    require(balance == "least_loaded" || balance == "priority" || balance == "weighted", "调度策略无效");
    for (const auto name : {"configPath","authPath"}) {
        auto path = settings.at(name).get<std::string>();
        require(path.empty() || path_from_utf8(path).is_absolute(), "配置文件必须使用绝对路径");
    }
    auto permission = settings.at("permission").get<std::string>();
    require(permission.size() <= 80 && std::all_of(permission.begin(), permission.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }), "权限预设只能包含字母、数字、连字符与下划线");
    auto limits = [](const Json& row) {
        require(row.at("enabled").is_boolean(), "启用字段必须为布尔值");
        require(row.at("concurrency").is_number_integer(), "并发必须为整数");
        const auto cap = row.at("concurrency").get<int>();
        require(cap >= 1 && cap <= 1000, "总并发必须在 1 至 1000 之间");
        require(row.at("roles").is_object(), "角色设置必须为对象");
        for (const auto& [role, value] : row.at("roles").items())
            require(roles.contains(role) && value.is_number_integer() && value.get<int>() >= 0 &&
                value.get<int>() <= cap, "角色并发必须在 0 和总并发之间");
    };
    for (const auto& account : settings.at("accounts")) limits(account);
    for (const auto& pool : settings.at("pools")) {
        limits(pool);
        require(settings.at("accounts").contains(pool.at("provider").get<std::string>()), "模型池的账号无效");
        require(pool.at("weight").is_number_integer() && pool.at("priority").is_number_integer() &&
            pool.at("weight").get<int>() >= 1 && pool.at("weight").get<int>() <= 10000, "优先级或权重无效");
        const auto effort = pool.at("effort").get<std::string>();
        const auto& efforts = pool.at("efforts");
        require(effort.empty() || std::find(efforts.begin(), efforts.end(), effort) != efforts.end(), "DSH 不提供该思考强度");
    }
}
Json public_row(Json row) {
    row.erase("owner");
    row.erase("dispatchOperation");
    return row;
}
std::string choose_pool(const Json& settings, const Json& sessions, const std::string& role, const std::string& pinned) {
    std::vector<std::tuple<int,int,std::string>> scores;
    for (const auto& [key, pool] : settings.at("pools").items()) {
        if (!pool.at("enabled").get<bool>() || (!pinned.empty() && key != pinned)) continue;
        const auto provider = pool.at("provider").get<std::string>();
        const auto& account = settings.at("accounts").at(provider);
        if (!account.at("enabled").get<bool>()) continue;
        int account_count = 0, account_role = 0, pool_count = 0, pool_role = 0;
        for (const auto& row : sessions) {
            if (!active(row)) continue;
            if (row.at("provider") == provider) {
                ++account_count;
                if (row.at("role") == role) ++account_role;
            }
            const auto legacy_model = row.value("source","") == "codex" && row.at("provider") == provider &&
                row.value("model","") == pool.at("model").get<std::string>();
            if (row.value("poolId","") == key || legacy_model) {
                ++pool_count;
                if (row.at("role") == role) ++pool_role;
            }
        }
        const auto cap = pool.at("concurrency").get<int>();
        const auto account_cap = account.at("concurrency").get<int>();
        if (account_count >= account_cap || account_role >= account.at("roles").value(role, account_cap) ||
            pool_count >= cap || pool_role >= pool.at("roles").value(role, 0)) continue;
        const auto load = 100000 * pool_count / cap;
        const auto priority = pool.at("priority").get<int>();
        const auto policy = settings.at("balance").get<std::string>();
        if (policy == "priority") scores.emplace_back(priority, load, key);
        else if (policy == "weighted") scores.emplace_back(100000 * (pool_count + 1) / pool.at("weight").get<int>(), priority, key);
        else scores.emplace_back(load, priority, key);
    }
    require(!scores.empty(), "没有符合角色及并发限制的启用模型池");
    return std::get<2>(*std::min_element(scores.begin(), scores.end()));
}
Json catalog_import(Json& settings, const Json& catalog) {
    int group_count = 0, model_count = 0;
    const auto& groups = catalog.at("groups");
    require(groups.is_array(), "DSH 模型目录格式无效");
    for (const auto& group : groups) {
        auto provider = field(group, "id", 512);
        auto& account = settings["accounts"][provider];
        if (account.is_null()) account = {{"id",provider},{"enabled",true},{"concurrency",1},{"roles",Json::object()}};
        account["name"] = group.value("name", provider);
        ++group_count;
        for (const auto& model : group.at("models")) {
            auto model_id = field(model, "id", 512);
            Json efforts = Json::array();
            Json reasoning = model.value("reasoning", Json::object());
            if (!reasoning.is_object()) reasoning = Json::object();
            for (const auto& value : reasoning.value("efforts", Json::array()))
                if (value.is_object() && value.contains("id") && value["id"].is_string()) efforts.push_back(value["id"]);
            auto& pools = settings["pools"];
            auto it = std::find_if(pools.begin(), pools.end(), [&](const Json& value) {
                return value.at("provider") == provider && value.at("model") == model_id;
            });
            if (it == pools.end()) {
                auto key = id();
                pools[key] = {{"id",key},{"provider",provider},{"model",model_id},{"enabled",false},
                    {"concurrency",1},{"priority",0},{"weight",1},{"effort",""},{"roles",{{"developer",1}}}};
                it = pools.find(key);
            }
            auto& pool = *it;
            pool["name"] = model.value("name", model_id);
            pool["efforts"] = efforts;
            pool["defaultEffort"] = reasoning.value("defaultEffort", Json(""));
            if (!pool.at("effort").get<std::string>().empty() &&
                std::find(efforts.begin(), efforts.end(), pool.at("effort")) == efforts.end()) {
                pool["effort"] = "";
                pool["enabled"] = false;
            }
            ++model_count;
        }
    }
    return {{"accounts",group_count},{"models",model_count}};
}
Json rpc_state(const Dsh& dsh, const Json& row) {
    require(row.contains("dshId") && row.at("dshId").is_string(), "会话尚未取得 DSH ID");
    require(row.value("owner", "") == dsh.owner(), "没有连接到该会话最初所属的 DSH 服务");
    return dsh.request("session/state", {{"sessionId",row.at("dshId")}});
}
Json report(const Dsh& dsh, const Json& row) {
    Json request{{"address",{{"kind","session"},{"sessionId",row.at("dshId")}}},{"maxMessages",8}};
    if (row.contains("cursor")) request["throughSeq"] = row.at("cursor");
    return dsh.request("session/page",request);
}
constexpr std::array usage_keys{"uncachedInputTokens","outputTokens","cacheReadTokens","cacheWriteTokens"};
std::int64_t midnight() {
    auto stamp = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local,&stamp) != 0) return 0;
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    return std::int64_t(std::mktime(&local)) * 1000;
}
std::optional<std::uint64_t> token_sample(const Json& event) {
    const auto& data=event.at("data");
    if (data.contains("usage") && data.at("usage").is_object()) {
        const auto& usage=data.at("usage");
        return usage.at("inputTokens").get<std::uint64_t>()+usage.at("outputTokens").get<std::uint64_t>()+
            usage.value("cacheReadTokens",std::uint64_t(0))+usage.value("cacheWriteTokens",std::uint64_t(0));
    }
    const auto stream=data.value("stream",Json::array());
    if (stream.is_array()) for (auto it=stream.rbegin();it!=stream.rend();++it) {
        if (!it->contains("chunk") || it->at("chunk").value("type","")!="usage") continue;
        const auto& usage=it->at("chunk").at("usage");
        return usage.at("inputTokens").get<std::uint64_t>()+usage.at("outputTokens").get<std::uint64_t>()+
            usage.value("cacheReadTokens",std::uint64_t(0))+usage.value("cacheWriteTokens",std::uint64_t(0));
    }
    return {};
}
std::uint64_t day_usage(const Dsh& dsh, const Json& row, std::int64_t cursor, std::int64_t start) {
    using Attempt=std::pair<int,int>;
    std::vector<std::pair<Attempt,std::optional<std::uint64_t>>> events;
    auto before=cursor+1;
    for (int pages=0;pages<32;++pages) {
        Json request{{"address",{{"kind","session"},{"sessionId",row.at("dshId")}}},
            {"throughSeq",cursor},{"maxMessages",16}};
        if (before<=cursor) request["beforeSeq"]=before;
        auto page=dsh.request("session/page",request);
        auto earliest=before;
        bool old=false;
        const auto& records=page.at("records");
        for (auto it=records.rbegin();it!=records.rend();++it) {
            if (!it->contains("event")) continue;
            const auto& event=it->at("event");
            auto seq=event.value("seq",before);
            if (seq>=before) continue;
            earliest=std::min(earliest,seq);
            auto stamp=event.value("time",std::int64_t(0));
            if (stamp>0 && stamp<start) {old=true;continue;}
            const auto kind=event.value("type","");
            if (kind!="assistant/message" && kind!="assistant/attempt" && kind!="llm/retry-started") continue;
            require(stamp>0,"用量事件没有时间");
            const auto& data=event.at("data");
            Attempt attempt{data.at("turn").get<int>(),data.at("step").get<int>()};
            if (kind=="llm/retry-started") events.emplace_back(attempt,std::nullopt);
            else if (auto sample=token_sample(event)) events.emplace_back(attempt,sample);
        }
        if (old || !page.value("hasMore",false)) {
            std::uint64_t total=0, previous=0;
            std::optional<Attempt> last;
            for (auto it=events.rbegin();it!=events.rend();++it) {
                const auto& [attempt,sample]=*it;
                if (sample) {
                    if (last==attempt) total-=previous;
                    total+=*sample;previous=*sample;last=attempt;
                } else if (last==attempt) last.reset();
            }
            return total;
        }
        require(earliest<before,"用量分页无法继续");
        before=earliest;
    }
    throw std::runtime_error("今日历史用量超过扫描范围");
}
Json telemetry_overview(const Json& sessions) {
    const auto today=midnight();
    std::uint64_t total=0;
    int missing=0;
    Json usage=Json::object();
    for (auto name:usage_keys) usage[name]=std::uint64_t(0);
    for (const auto& row:sessions) {
        auto tokens=row.value("usage",Json::object());
        for (auto name:usage_keys) usage[name]=usage.at(name).get<std::uint64_t>()+tokens.value(name,std::uint64_t(0));
        if (row.value("tokenDay",std::int64_t(0))==today && row.contains("todayTokens")) total+=row.at("todayTokens").get<std::uint64_t>();
        else if (row.contains("startCursor") && (active(row) || row.value("completed",std::int64_t(0))>=today)) ++missing;
    }
    const auto hours=double(now()-today)/3600000.0;
    return {{"total",usage},{"todayTokens",total},{"hourlyTokens",hours>0?Json(double(total)/hours):Json(nullptr)},{"missing",missing}};
}
Json refresh(const fs::path& data, const std::string& key) {
    Json settings, prior;
    {
        Database db(data, false);
        settings = db.state.at("settings");
        prior = db.state.at("sessions").at(key);
    }
    if (!prior.contains("startCursor")) return public_row(prior);
    Dsh dsh(path_from_utf8(settings.at("authPath").get<std::string>()));
    auto live = rpc_state(dsh, prior);
    auto cursor = live.at("cursor").get<std::int64_t>();
    const auto start = prior.at("startCursor").get<std::int64_t>();
    std::string status;
    std::string version;
    auto completed = now();
    if (!live.value("pendingInput", Json::array()).empty()) status = "waiting_input";
    else if (live.at("running").get<bool>()) status = "running";
    else if (!live.value("queue", Json::array()).empty()) status = "queued";
    else if (!active(prior) && prior.value("cursor", start) == cursor) return public_row(prior);
    else if (cursor <= start) status = "queued";
    else {
        status = "unknown";
        const auto page = dsh.request("session/page", {{"address",{{"kind","session"},{"sessionId",prior.at("dshId")}}},
            {"throughSeq",cursor},{"maxMessages",4}});
        const auto& records = page.at("records");
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (!it->contains("event")) continue;
            const auto& event = it->at("event");
            auto seq = event.value("seq", std::int64_t(-1));
            if (seq <= start || event.value("type", "") == "turn/start") break;
            if (event.value("type", "") != "turn/end") continue;
            const auto reason = event.at("data").at("reason").value("kind","");
            status = reason == "completed" ? "succeeded"
                : reason == "aborted" || reason == "cancelled" ? "cancelled" : "failed";
            version = std::to_string(start) + ":" + std::to_string(seq);
            completed = event.value("time", completed);
            break;
        }
    }
    const auto today=midnight();
    Json usage;
    std::optional<std::uint64_t> tokens;
    try {
        const auto projections=dsh.request("session/projections",{{"sessionId",prior.at("dshId")}});
        usage=projections.at("/values/tokenUsage"_json_pointer);
        std::uint64_t sum=0;
        for (auto name:usage_keys) sum+=usage.at(name).get<std::uint64_t>();
        if (prior.value("created",std::int64_t(0))>=today) tokens=sum;
        else if (prior.value("tokenDay",std::int64_t(0))==today && prior.value("cursor",start)==cursor &&
                 prior.contains("todayTokens")) tokens=prior.at("todayTokens").get<std::uint64_t>();
        else tokens=day_usage(dsh,prior,cursor,today);
    } catch (const std::exception&) {}
    Database db(data, true);
    auto& row = db.state["sessions"].at(key);
    if (row.value("cursor", start) <= cursor && row.at("startCursor") == start) {
        row["status"] = status;
        row["cursor"] = cursor;
        if (usage.is_object()) row["usage"]=usage;
        if (tokens) {row["tokenDay"]=today;row["todayTokens"]=*tokens;}
        if (row.value("resultVersion","") != version) {
            row["resultVersion"] = version;
            row["read"] = false;
            row["processed"] = false;
            row["completed"] = version.empty() ? 0 : completed;
        }
        row["updated"] = now();
        db.save();
    }
    return public_row(row);
}
}

Cluster::Cluster(fs::path data) : data_(std::move(data)) {
    if (data_.empty()) data_ = environment(L"FATFISHBOX_DATA_DIR");
    if (data_.empty()) data_ = fs::path(environment(L"LOCALAPPDATA")) / L"FatFishBox";
    require(data_.is_absolute(), "应用数据目录必须使用绝对路径");
    fs::create_directories(data_);
}

namespace {
Json session_overview(const Json& settings,const Json& sessions,const LegacySessions& legacy) {
    int running=0, pending=0;
    for (const auto& row:sessions) {
        if (active(row)) ++running;
        else if (!row.value("processed",false)) ++pending;
    }
    return {{"active",running},{"unprocessed",pending},{"accounts",settings.at("accounts").size()},
        {"models",settings.at("pools").size()},{"connected",!settings.at("authPath").get<std::string>().empty()},
        {"codexSessions",legacy.rows.size()},{"codexSources",legacy.sources},{"unreadableCodexSources",legacy.unreadable},
        {"usage",telemetry_overview(sessions)}};
}
}

Json Cluster::desktop_snapshot() const {
    Database db(data_,false);
    const auto& settings=db.state.at("settings");
    auto sessions=db.state.at("sessions");
    const auto legacy=read_legacy_sessions(settings);
    sessions.update(legacy.rows);
    std::vector<Json> running,pending;
    for (const auto& row:sessions) {
        if (active(row)) running.push_back(public_row(row));
        else if (!row.value("processed",false)) pending.push_back(public_row(row));
    }
    const auto order=[](const Json& row,const char* stamp) {
        return std::pair(row.value(stamp,std::int64_t(0)),row.at("id").get<std::string>());
    };
    std::sort(running.begin(),running.end(),[&](const Json& a,const Json& b) {return order(a,"updated")>order(b,"updated");});
    std::sort(pending.begin(),pending.end(),[&](const Json& a,const Json& b) {return order(a,"completed")<order(b,"completed");});
    return {{"overview",session_overview(settings,sessions,legacy)},{"settings",settings},
        {"running",std::move(running)},{"pending",std::move(pending)}};
}

Json Cluster::invoke(std::string_view tool, const Json& input) const {
    require(input.is_object(), "工具参数必须为 JSON 对象");
    if (tool == "cluster_overview" || tool == "cluster_get_settings") {
        Database db(data_, false);
        const auto& settings = db.state.at("settings");
        if (tool == "cluster_get_settings") return settings;
        auto sessions = db.state.at("sessions");
        const auto legacy = read_legacy_sessions(settings);
        sessions.update(legacy.rows);
        return session_overview(settings,sessions,legacy);
    }
    if (tool == "cluster_next_handoff") {
        require(std::ranges::all_of(input.items(),[](const auto& entry) {
            return entry.key()=="claimToken" || entry.key()=="outcome";
        }),"只能凭当前领取凭据提交处理结果");
        const auto token=input.contains("claimToken")?field(input,"claimToken",100):"";
        const auto outcome=input.contains("outcome")?field(input,"outcome",2000):"";
        require(!input.contains("outcome") || outcome.find_first_not_of(" \t\r\n")!=std::string::npos,"请说明实际采取的处置");
        require(outcome.empty() || !token.empty(),"提交处理结果需要当前领取凭据");
        const auto queue_file=data_/L"handoff.json";
        const auto previous=fs::exists(queue_file)?read_json(queue_file):Json::object();
        const auto previous_claim=previous.value("claim",Json::object());
        if (previous_claim.empty() || previous_claim.value("expiresAt",std::int64_t(0))<=now() ||
            (!outcome.empty() && previous_claim.value("claimToken","")==token))
            invoke("cluster_refresh_sessions");
        const ExclusiveStateLock lock(data_/L"handoff.lock");
        auto queue=fs::exists(queue_file)?read_json(queue_file):Json::object();
        auto claim=queue.value("claim",Json::object());
        auto snapshot=[&] {
            Database db(data_,false);
            auto rows=db.state.at("sessions");
            rows.update(read_legacy_sessions(db.state.at("settings")).rows);
            return rows;
        };
        auto present=[&](const Json& current) {
            const auto key=current.at("sessionId").get<std::string>();
            const auto row=invoke("cluster_refresh_session",{{"sessionId",key}});
            require(row.at("resultVersion")==current.at("resultVersion") && !active(row),"领取的结果版本已变化");
            const auto report=invoke("cluster_read_result",{{"sessionId",key}});
            require(report.at("resultVersion")==current.at("resultVersion"),"领取的结果版本已变化");
            return Json{{"claimToken",current.at("claimToken")},{"expiresAt",current.at("expiresAt")},
                {"session",row},{"result",report.at("messages")}};
        };
        if (!claim.empty() && claim.value("expiresAt",std::int64_t(0))<=now()) {
            claim=Json::object();
            queue.erase("claim");
            write_state_file(queue_file,queue);
        }
        if (!token.empty() && token==claim.value("claimToken","") && outcome.empty()) return present(claim);
        if (!token.empty() && token==queue.value("lastToken","")) {
            if (!claim.empty() && queue.value("nextToken","")==claim.value("claimToken","")) {
                auto response=present(claim);
                response["completed"]=true;
                return response;
            }
            return {{"completed",true},{"busy",!claim.empty()}};
        }
        if (!claim.empty()) {
            if (claim.at("claimToken")!=token) return {{"busy",true},{"expiresAt",claim.at("expiresAt")}};
            const auto key=claim.at("sessionId").get<std::string>();
            const auto version=claim.at("resultVersion").get<std::string>();
            if (!key.starts_with("codex:")) refresh(data_,key);
            const auto rows=snapshot();
            if (!rows.contains(key) || rows.at(key).at("resultVersion")!=version || active(rows.at(key))) {
                queue.erase("claim");
                write_state_file(queue_file,queue);
                return {{"changed",true}};
            }
            if (key.starts_with("codex:")) {
                const auto settings=invoke("cluster_get_settings");
                complete_legacy_handoff(settings,key,version,outcome);
            } else {
                Database db(data_,true);
                auto& row=db.state["sessions"].at(key);
                require(!active(row) && row.at("resultVersion")==version,"会话的结果版本已变化");
                row["read"]=true;
                row["processed"]=true;
                row["handoffOutcome"]=outcome;
                db.save();
            }
            queue["lastToken"]=token;
            queue.erase("nextToken");
            queue.erase("claim");
            write_state_file(queue_file,queue);
        } else require(token.empty(),"领取凭据过期，请重新领取");

        const auto rows=snapshot();
        const Json* oldest=nullptr;
        const auto sort_key=[](const Json& row) {
            return std::pair(row.value("completed",std::int64_t(0)),row.at("id").get<std::string>());
        };
        for (const auto& row:rows) {
            if (active(row) || row.value("processed",false) || row.value("resultVersion","").empty()) continue;
            if (!oldest || sort_key(row)<sort_key(*oldest)) oldest=&row;
        }
        if (!oldest) return {{"empty",true},{"completed",!token.empty()}};
        claim={{"source",oldest->value("source","fatfishbox")},{"sessionId",oldest->at("id")},
            {"resultVersion",oldest->at("resultVersion")},{"claimToken",id()},
            {"expiresAt",now()+30*60*1000}};
        auto result=present(claim);
        queue["claim"]=claim;
        if (!token.empty()) queue["nextToken"]=claim.at("claimToken");
        write_state_file(queue_file,queue);
        result["completed"]=!token.empty();
        return result;
    }
    if (tool == "cluster_get_presets" || tool == "cluster_import_models") {
        Json settings;
        {
            Database db(data_, false);
            settings = db.state.at("settings");
        }
        const auto connection = discover(settings, input);
        if (tool == "cluster_get_presets") {
            Json permissions=Json::array(), agents=Json::array();
            const auto append=[](Json& list,const std::string& key,const std::string& label) {
                if (!key.empty() && std::ranges::none_of(list,[&](const Json& row){return row.at("id")==key;}))
                    list.push_back({{"id",key},{"label",label.empty()?key:label}});
            };
            if (!connection.auth.empty()) {
                Dsh dsh(connection.auth);
                const auto catalog=dsh.request("permissionPresets/catalog");
                for (const auto& row:catalog.at("defaultOptions"))
                    append(permissions,row.at("value").get<std::string>(),row.value("name",""));
                const auto roster=dsh.request("agentPresets/list");
                for (const auto& row:roster.at("presets"))
                    if (row.value("broken","").empty())
                        append(agents,row.at("id").get<std::string>(),row.value("name",""));
            } else {
                append(permissions,connection.permission,"");
                append(agents,connection.preset,"");
            }
            return {{"permissions",permissions},{"agents",agents},{"connected",!connection.auth.empty()}};
        }
        require(!connection.auth.empty(), "请指定已授权的 DSH 配置或本机授权文件");
        const auto catalog = Dsh(connection.auth).request("session/modelCatalog");
        auto summary = catalog_import(settings, catalog);
        settings["authPath"] = utf8(connection.auth);
        settings["configPath"] = utf8(connection.config);
        if (settings["permission"] == "") settings["permission"] = connection.permission;
        if (settings["preset"] == "") settings["preset"] = connection.preset;
        validate_configuration(settings);
        Database db(data_, true);
        require(db.state["settings"]["revision"] == settings["revision"], "设置已变化，请重试导入");
        settings["revision"] = settings["revision"].get<int>() + 1;
        db.state["settings"] = settings;
        db.save();
        summary["settings"] = settings;
        return summary;
    }
    if (tool == "cluster_configure" || tool == "cluster_update_pool" || tool == "cluster_update_account") {
        Database db(data_, true);
        auto& settings = db.state["settings"];
        require(settings.at("revision") == input.at("revision"), "配置已变化，请刷新后重试");
        if (tool == "cluster_configure") {
            for (const auto member : {"configPath","authPath","permission","preset","balance"})
                if (input.contains(member)) settings[member] = input.at(member);
        } else {
            const bool pool = tool == "cluster_update_pool";
            auto& entry = settings.at(pool ? "pools" : "accounts").at(field(input, "id", 512));
            for (const auto member : {"enabled","concurrency","roles"})
                if (input.contains(member)) entry[member] = input.at(member);
            if (pool) for (const auto member : {"priority","weight","effort"})
                if (input.contains(member)) entry[member] = input.at(member);
        }
        validate_configuration(settings);
        settings["revision"] = settings["revision"].get<int>() + 1;
        db.save();
        return settings;
    }
    if (tool == "cluster_start_session") {
        const auto title = field(input,"title",400), batch = field(input,"batch",400);
        const auto role = field(input,"role",32), task = field(input,"task",64000);
        require(roles.contains(role), "未知角色");
        const auto work = path_from_utf8(field(input,"workspace",4000));
        require(work.is_absolute() && fs::is_directory(work) && work.has_relative_path(), "工作区必须是现有的绝对子目录");
        const auto workspace = fs::canonical(work);
        require(workspace.has_relative_path(), "工作区不能指向磁盘根目录");
        const auto key = id();
        Json settings, pool, row;
        {
            Database db(data_, true);
            settings = db.state.at("settings");
            require(settings.at("permission") != "", "派发前需要设置并验证 DSH 权限预设");
            auto& sessions = db.state["sessions"];
            auto occupied = sessions;
            occupied.update(read_legacy_sessions(settings).rows);
            for (const auto& value : occupied) {
                if (active(value)) require(_wcsicmp(widen(value.at("workspace").get<std::string>()).c_str(),
                    workspace.c_str()) != 0, "该工作区已有活动会话");
            }
            auto pool_id = choose_pool(settings, occupied, role, input.value("poolId", ""));
            pool = settings.at("pools").at(pool_id);
            row = {{"id",key},{"title",title},{"role",role},{"batch",batch},{"workspace",utf8(workspace)},
                {"poolId",pool_id},{"provider",pool.at("provider")},{"model",pool.at("model")},{"status","starting"},
                {"created",now()},{"updated",now()},{"resultVersion",""},{"read",false},{"processed",false}};
            sessions[key] = row;
            db.save();
        }
        bool may_have_dispatched = false;
        std::string error;
        try {
            Dsh dsh(path_from_utf8(settings.at("authPath").get<std::string>()));
            const auto catalog = dsh.request("session/modelCatalog");
            Json live_model;
            for (auto& group : catalog.at("groups"))
                if (group.at("id") == pool.at("provider"))
                    for (auto& model : group.at("models"))
                        if (model.at("id") == pool.at("model")) live_model = model;
            require(live_model.is_object(), "账号和模型已不在 DSH 实时目录中");
            auto effort = pool.at("effort").get<std::string>();
            const auto reasoning = live_model.value("reasoning", Json::object());
            if (effort.empty() && reasoning.is_object() && reasoning.contains("defaultEffort") && reasoning.at("defaultEffort").is_string())
                effort = reasoning.at("defaultEffort").get<std::string>();
            if (!effort.empty()) {
                bool found = false;
                if (reasoning.is_object()) for (const auto& value : reasoning.value("efforts", Json::array()))
                    if (value.value("id","") == effort) found = true;
                require(found, "思考强度已不在 DSH 实时目录中");
            }
            Json create{{"cwd",utf8(workspace)}};
            if (settings.at("preset") != "") create["agentPreset"] = settings.at("preset");
            const auto created = dsh.request("session/create", create);
            const auto sid = field(created, "sessionId", 200);
            row["dshId"] = sid;
            row["owner"] = dsh.owner();
            if (created.contains("agentPreset") && settings.at("preset") != "")
                require(created.at("agentPreset") == settings.at("preset"), "DSH 未保留 Agent 预设");
            const auto permission = settings.at("permission").get<std::string>();
            const auto applied = dsh.call("commands/execute", {{"agentId",sid},{"line","/permission " + permission},
                {"submittedAttachments",Json::array()}});
            require(applied.value("result", applied).value("kind","") == "success", "DSH 没有应用权限预设");
            bool verified = false;
            for (int attempt = 0; attempt < 8 && !verified; ++attempt) {
                const auto value = dsh.request("session/projections", {{"sessionId",sid}});
                const auto path = "/values/permissions/currentValue"_json_pointer;
                verified = value.contains(path) && value.at(path) == permission;
                if (!verified) Sleep(100);
            }
            require(verified, "DSH 权限预设复核失败");
            Json select{{"sessionId",sid},{"provider",pool.at("provider")},{"model",pool.at("model")}};
            if (!effort.empty()) select["reasoningEffort"] = effort;
            const auto result = dsh.request("session/selectModel", select);
            const auto& selected = result.at("selected");
            require(selected.at("provider") == pool.at("provider") && selected.at("model") == pool.at("model") &&
                (effort.empty() || selected.value("reasoningEffort","") == effort), "DSH 所选模型或思考强度与配置不符");
            const auto live = rpc_state(dsh, row);
            row["startCursor"] = live.at("cursor");
            row["cursor"] = live.at("cursor");
            row["effort"] = effort;
            {
                Database db(data_, true);
                db.state["sessions"][key] = row;
                db.save();
            }
            may_have_dispatched = true;
            const auto prompt = "Work in the assigned workspace only. Do not change provider/model, start nested agents, "
                "or publish changes without authorization. Return verifiable evidence.\n\n" + task;
            const auto reply = dsh.request("session/prompt", {{"sessionId",sid},{"requestId",key},{"mode","queue"},
                {"content",Json::array({{{"type","text"},{"text",prompt}}})}});
            may_have_dispatched = reply.value("accepted",true);
            require(may_have_dispatched, "DSH 拒绝了任务");
            row["status"] = "queued";
        } catch (const std::exception& e) {
            error = e.what();
            row["status"] = may_have_dispatched ? "unknown" : "failed";
            row["error"] = error;
            if (!may_have_dispatched) {
                row["resultVersion"] = "dispatch:failed";
                row["completed"] = now();
                row.erase("startCursor");
            }
        }
        Database db(data_, true);
        auto& stored = db.state["sessions"].at(key);
        if (stored.at("status") == "starting" || row.at("status") == "failed") {
            stored = row;
            db.save();
        }
        return public_row(stored);
    }
    if (tool == "cluster_refresh_sessions") {
        const auto limit = input.value("limit", 12);
        require(limit >= 1 && limit <= 20, "同步数量必须在 1 至 20 之间");
        std::vector<std::pair<std::int64_t,std::string>> candidates;
        {
            Database db(data_, false);
            for (const auto& row : db.state.at("sessions"))
                if (active(row) && row.contains("startCursor"))
                    candidates.emplace_back(row.at("updated").get<std::int64_t>(), row.at("id").get<std::string>());
        }
        std::sort(candidates.begin(),candidates.end());
        Json sessions=Json::array(), errors=Json::array();
        for (std::size_t i=0; i<std::min(std::size_t(limit),candidates.size()); ++i) {
            const auto& current=candidates[i].second;
            try { sessions.push_back(refresh(data_,current)); }
            catch (const std::exception& e) { errors.push_back({{"sessionId",current},{"error",e.what()}}); }
        }
        return {{"sessions",sessions},{"errors",errors},{"total",candidates.size()}};
    }
    const auto key = field(input,"sessionId",100);
    if (key.starts_with("codex:")) {
        Database db(data_, false);
        const auto& settings = db.state.at("settings");
        if (tool == "cluster_read_result") return legacy_result(settings,key);
        if (tool == "cluster_get_session" || tool == "cluster_refresh_session")
            return read_legacy_sessions(settings).rows.at(key);
        throw std::runtime_error("此会话属于原 Codex 插件，请在原插件中取消或确认");
    }
    if (tool == "cluster_refresh_session") return refresh(data_,key);
    if (tool == "cluster_mark_receipt") {
        refresh(data_, key);
        Database db(data_, true);
        auto& row = db.state["sessions"].at(key);
        require(!active(row) && row.at("resultVersion") == field(input,"resultVersion",100), "结果版本已变化，无法确认");
        row["read"] = true;
        row["processed"] = row.at("processed").get<bool>() || input.value("processed", false);
        db.save();
        return public_row(row);
    }
    Json settings, row;
    {
        Database db(data_, false);
        settings = db.state.at("settings");
        row = db.state.at("sessions").at(key);
    }
    if (tool == "cluster_get_session") return public_row(row);
    Dsh dsh(path_from_utf8(settings.at("authPath").get<std::string>()));
    rpc_state(dsh, row);
    if (tool == "cluster_cancel_session") {
        require(active(row) && row.contains("startCursor"), "只能取消已派发的活动会话");
        dsh.request("session/cancel", {{"sessionId",row.at("dshId")}});
        return refresh(data_,key);
    }
    if (tool == "cluster_read_result") {
        const auto snapshot = report(dsh, row);
        Json messages = Json::array();
        std::size_t remaining = 6000;
        const auto& records = snapshot.at("records");
        for (auto it = records.rbegin(); it != records.rend() && remaining; ++it) {
            if (!it->contains("event")) continue;
            const auto& event = it->at("event");
            if (row.contains("startCursor") && event.value("seq",std::int64_t(-1))<=row.at("startCursor").get<std::int64_t>())
                continue;
            const auto type = event.value("type","");
            if (type != "assistant/message" && type != "user/message") continue;
            const auto path = "/data/message/content"_json_pointer;
            if (!event.contains(path)) continue;
            const auto& content = event.at(path);
            std::string text;
            if (content.is_string()) text = content.get<std::string>();
            else if (content.is_array()) for (const auto& part : content)
                if (part.is_object() && part.contains("text") && part.at("text").is_string()) text += part.at("text").get<std::string>();
            if (text.empty()) continue;
            const bool truncated = text.size() > remaining;
            if (truncated) {
                auto start = text.size() - remaining;
                while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
                text.erase(0, start);
            }
            remaining -= text.size();
            messages.push_back({{"type",type},{"text",text},{"truncated",truncated}});
        }
        std::reverse(messages.begin(), messages.end());
        return {{"sessionId",key},{"resultVersion",row.at("resultVersion")},{"messages",messages}};
    }
    throw std::runtime_error("未知肥鱼重工操作");
}

Json Cluster::tools() {
    Json tools = Json::array();
    const Json text{{"type","string"}}, integer{{"type","integer"}}, boolean{{"type","boolean"}},
        object{{"type","object"}};
    auto add = [&](const char* name, const char* description, Json fields = Json::object(), Json required = Json::array()) {
        tools.push_back({{"name",name},{"description",description},
            {"inputSchema",{{"type","object"},{"properties",fields},{"required",required},{"additionalProperties",false}}}});
    };
    add("cluster_overview","查看活动会话、待交接、未读和模型账号汇总。");
    add("cluster_get_settings","读取当前连接配置、账号、模型池与 revision。");
    add("cluster_get_presets","读取 DSH 权限预设和可用 Agent 预设。",
        {{"authPath",text},{"configPath",text}});
    add("cluster_import_models","从 DSH 实时目录导入所有已配置账号和模型。",
        {{"authPath",text},{"configPath",text}});
    add("cluster_configure","更新 DSH 连接设置和全局负载均衡策略。",
        {{"revision",integer},{"configPath",text},{"authPath",text},{"permission",text},{"preset",text},{"balance",text}}, {"revision"});
    add("cluster_update_account","设置一个账号的启用状态、总并发和各角色并发。",
        {{"revision",integer},{"id",text},{"enabled",boolean},{"concurrency",integer},{"roles",object}}, {"revision","id"});
    add("cluster_update_pool","设置模型、角色并发、思考强度、调度优先级和权重。",
        {{"revision",integer},{"id",text},{"enabled",boolean},{"concurrency",integer},{"roles",object},
         {"effort",text},{"priority",integer},{"weight",integer}}, {"revision","id"});
    add("cluster_next_handoff","获取下一个需处理对话。完成实际处置后，传回 claimToken 和 outcome 自动确认并领取下一条。省略 outcome 可用领取凭据重读本条。",
        {{"claimToken",text},{"outcome",text}});
    tools.back()["title"]="获取下一个需处理对话";
    add("cluster_start_session","在授权工作区和角色并发范围内派发一个会话。",
        {{"title",text},{"batch",text},{"role",text},{"workspace",text},{"task",text},{"poolId",text}},
        {"title","batch","role","workspace","task"});
    add("cluster_refresh_sessions","同步本地登记的最早活动会话，每次最多 20 个。",{{"limit",integer}});
    add("cluster_cancel_session","取消已知的活动会话。",{{"sessionId",text}},{"sessionId"});
    return tools;
}
}
