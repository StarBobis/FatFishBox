#include "cluster.h"
#include "dsh.h"
#include "legacy.h"
#include "state_file.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <atomic>
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
        {"accounts",Json::object()},{"pools",Json::object()}};
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
            auto& settings=state.at("settings");
            settings.erase("balance");
            for (auto& row:settings.at("accounts")) {
                row.erase("roles");
                row.erase("enabled");
                row.erase("concurrency");
            }
            for (auto& row:settings.at("pools")) {
                row.erase("roles");
                row.erase("priority");
                row.erase("weight");
            }
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
bool commit_sha(std::string_view sha) {
    return (sha.size()==40 || sha.size()==64) && std::ranges::all_of(sha,[](unsigned char ch) {
        return (ch>='0' && ch<='9') || (ch>='a' && ch<='f') || (ch>='A' && ch<='F');
    });
}
bool evidence_paths(const Json& paths) {
    return paths.is_array() && paths.size()<=16 && std::ranges::all_of(paths,[](const Json& path) {
        if (!path.is_string()) return false;
        const auto& value=path.get_ref<const std::string&>();
        return !value.empty() && value.size()<=1000 && value.find('\0')==std::string::npos &&
            path_from_utf8(value).is_absolute();
    });
}
Json extract_delivery(Json& messages) {
    for (auto it=messages.rbegin();it!=messages.rend();++it) {
        const auto kind=it->value("type","");
        if (kind!="assistant/message" && kind!="assistant/response") continue;
        auto output=it->value("text","");
        constexpr std::string_view fence="```fatfish-delivery";
        const auto begin=output.rfind(fence);
        if (begin==std::string::npos) continue;
        const auto after=begin+fence.size();
        const auto end=output.find("```",after);
        if (end==std::string::npos || end-after>6000) continue;
        const auto parsed=Json::parse(std::string_view(output).substr(after,end-after),nullptr,false);
        if (!parsed.is_object()) continue;
        const auto sha=parsed.value("candidateSha",Json());
        const auto clean=parsed.value("worktreeClean",Json());
        const auto tests=parsed.value("tests",Json());
        const auto paths=parsed.value("evidencePaths",Json());
        if (!parsed.contains("candidateSha") || (!sha.is_null() && (!sha.is_string() || !commit_sha(sha.get_ref<const std::string&>()))) ||
            !clean.is_boolean() || !tests.is_array() || tests.size()>16 || !evidence_paths(paths)) continue;
        Json checks=Json::array();
        for (const auto& test:tests) {
            if (!test.is_object()) break;
            const auto command=test.value("command",Json()),code=test.value("exitCode",Json());
            if (!command.is_string() || command.get_ref<const std::string&>().empty() ||
                command.get_ref<const std::string&>().size()>600 || !code.is_number_integer()) break;
            checks.push_back({{"command",command},{"exitCode",code}});
        }
        if (tests.size()!=checks.size()) continue;
        output.erase(begin,end+3-begin);
        (*it)["text"]=std::move(output);
        return {{"candidateSha",sha},{"worktreeClean",clean},{"tests",checks},{"evidencePaths",paths},{"source","agent_reported"}};
    }
    return nullptr;
}
void validate_configuration(const Json& settings) {
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
    };
    for (const auto& pool : settings.at("pools")) {
        limits(pool);
        require(settings.at("accounts").contains(pool.at("provider").get<std::string>()), "模型池的账号无效");
        require(pool.at("effort").is_string(), "思考强度必须为字符串");
    }
}
void set_model_field(Json& pool,const std::string& field,const Json& value) {
    require(field=="enabled" || field=="concurrency" || field=="effort","未知模型设置");
    if (field!="enabled") require(pool.at("enabled").get<bool>(),"请先启用模型");
    if (field=="effort") {
        const auto effort=value.get<std::string>();
        const auto& efforts=pool.at("efforts");
        require(effort.empty() || std::find(efforts.begin(),efforts.end(),effort)!=efforts.end(),"DSH 不提供该思考强度");
    }
    pool[field]=value;
}
Json public_row(Json row) {
    row.erase("owner");
    row.erase("dispatchOperation");
    return row;
}
using CatalogModels=std::map<std::pair<std::string,std::string>,Json>;
CatalogModels catalog_models(const Json& catalog) {
    CatalogModels models;
    for (const auto& group:catalog.at("groups")) {
        if (!group.value("enabled",true) || !group.value("available",true)) continue;
        for (const auto& model:group.at("models"))
            if (model.value("enabled",true) && model.value("available",true))
                models.emplace(std::pair(group.at("id").get<std::string>(),model.at("id").get<std::string>()),model);
    }
    return models;
}

std::set<std::string> batch_allowlist(const Json& input) {
    if (!input.contains("allowedPoolIds")) return {};
    const auto& pools=input.at("allowedPoolIds");
    require(pools.is_array() && !pools.empty() && pools.size()<=64,"批次允许池需要 1 至 64 个池 ID");
    std::set<std::string> keys;
    for (const auto& item:pools) {
        require(item.is_string() && !item.get_ref<const std::string&>().empty() &&
            item.get_ref<const std::string&>().size()<=100,"批次允许池 ID 无效");
        keys.insert(item.get<std::string>());
    }
    return keys;
}

struct PoolSelection {
    std::string key;
    Json diagnostics;
};
PoolSelection choose_pool(const Json& settings,const Json& sessions,const CatalogModels& models,
    const std::set<std::string>& allowed,const std::string& pinned,const Json& cooldowns,
    const std::map<std::string,std::string>& rejected) {
    std::vector<std::tuple<int,int,std::string>> scores;
    Json diagnostics{{"pools",Json::array()},{"availablePools",Json::array()}};
    for (const auto& [key, pool] : settings.at("pools").items()) {
        const auto provider = pool.at("provider").get<std::string>();
        const auto model=pool.at("model").get<std::string>();
        const auto enabled=pool.at("enabled").get<bool>();
        const bool in_batch=allowed.empty() || allowed.contains(key);
        if (!enabled && key!=pinned && !allowed.contains(key)) continue;
        int account_count=0,pool_count=0,unverified=0;
        for (const auto& row : sessions) {
            if (!active(row)) continue;
            if (row.at("provider")==provider) ++account_count;
            const auto legacy_model = row.value("source","") == "codex" && row.at("provider") == provider &&
                row.value("model","") == pool.at("model").get<std::string>();
            if (row.value("poolId","")==key || legacy_model) {
                ++pool_count;
                if (row.value("stale",false)) ++unverified;
            }
        }
        const auto cap = pool.at("concurrency").get<int>();
        const auto cooldown=cooldowns.value(provider,std::int64_t(0));
        const auto refusal=rejected.find(key);
        const std::string_view reason=!in_batch?"batch_restricted":!enabled?"disabled":
            cooldown>now()?"account_cooldown":!models.contains({provider,model})?"model_unavailable":
            refusal!=rejected.end()?std::string_view(refusal->second):pool_count>=cap?"slots_full":"available";
        Json diagnostic{{"poolId",key},{"account",settings.at("accounts").at(provider).value("name",provider)},
            {"model",pool.value("name",model)},{"occupied",pool_count},{"concurrency",cap},{"reason",reason}};
        if (unverified) diagnostic["unverifiedSlots"]=unverified;
        if (reason=="account_cooldown") diagnostic["retryAt"]=cooldown;
        if (diagnostics.at("pools").size()<64 || key==pinned) diagnostics["pools"].push_back(diagnostic);
        if (reason!="available") continue;
        if (diagnostics.at("availablePools").size()<24)
            diagnostics["availablePools"].push_back(diagnostic);
        if (!pinned.empty() && key!=pinned) continue;
        const auto load = 100000 * pool_count / cap;
        scores.emplace_back(account_count,load,key);
    }
    if (!pinned.empty() && !settings.at("pools").contains(pinned))
        diagnostics["pools"].push_back({{"poolId",pinned},{"reason","unknown_pool"}});
    return {scores.empty()?"":std::get<2>(*std::min_element(scores.begin(),scores.end())),std::move(diagnostics)};
}

std::string selection_rejection(const DshError& error) {
    if (error.code=="http_429" || error.code=="rate_limited" || error.code=="account_cooldown" ||
        error.code=="quota_exceeded") return "account_cooldown";
    if (error.code=="model_unavailable" || error.code=="model_not_found" ||
        error.code=="provider_unavailable" || error.code=="provider_not_found") return "model_unavailable";
    return {};
}

std::string pool_failure(const Json& diagnostics,const std::string& pinned) {
    const auto& pools=diagnostics.at("pools");
    const auto selected=std::ranges::find_if(pools,[&](const Json& entry) {return entry.at("poolId")==pinned;});
    const auto describe=[](const Json& entry) {
        const auto reason=entry.at("reason").get<std::string>();
        if (reason=="unknown_pool") return std::string("池 ID 不存在");
        const auto name=entry.at("account").get<std::string>()+" / "+entry.at("model").get<std::string>();
        if (reason=="slots_full") return name+" 槽位已满（"+entry.at("occupied").dump()+"/"+entry.at("concurrency").dump()+"）";
        if (reason=="account_cooldown") return name+" 的账号正在冷却";
        if (reason=="batch_restricted") return name+" 不在批次允许池清单中";
        if (reason=="model_unavailable") return name+" 在 DSH 中不可用";
        return name+" 未启用";
    };
    std::string message;
    if (selected!=pools.end()) message=describe(*selected);
    else {
        for (const auto& entry:pools) {
            if (entry.at("reason")=="batch_restricted") continue;
            if (!message.empty()) message+="。";
            message+=describe(entry);
            if (message.size()>400) break;
        }
        if (message.empty()) message="派发范围内没有启用的模型池";
    }
    const auto& available=diagnostics.at("availablePools");
    if (!available.empty()) {
        message+="。可用池";
        for (std::size_t i=0;i<std::min<std::size_t>(3,available.size());++i) {
            const auto& entry=available.at(i);
            message+=(i?", ":" ")+entry.at("account").get<std::string>()+" / "+entry.at("model").get<std::string>()+
                " ("+entry.at("poolId").get<std::string>()+")";
        }
    }
    return message;
}
Json catalog_import(Json& settings, const Json& catalog) {
    int group_count = 0, model_count = 0;
    const auto& groups = catalog.at("groups");
    require(groups.is_array(), "DSH 模型目录格式无效");
    for (const auto& group : groups) {
        auto provider = field(group, "id", 512);
        auto& account = settings["accounts"][provider];
        if (account.is_null()) account = {{"id",provider}};
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
                    {"concurrency",1},{"effort",""}};
                it = pools.find(key);
            }
            auto& pool = *it;
            pool["name"] = model.value("name", model_id);
            pool["efforts"] = efforts;
            pool["defaultEffort"] = reasoning.value("defaultEffort", Json(""));
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
Json legacy_usage_cache(const fs::path& data) {
    try {
        auto cache=read_json(data/L"legacy-usage.json",8*1024*1024);
        if (cache.is_object()) return cache;
    } catch (const std::exception&) {}
    return Json::object();
}
Json telemetry_overview(const Json& sessions,const Json& legacy_cache) {
    const auto today=midnight();
    std::uint64_t total=0;
    int missing=0,sampled=0,today_sampled=0;
    Json usage=Json::object();
    for (auto name:usage_keys) usage[name]=std::uint64_t(0);
    for (const auto& row:sessions) {
        const Json* sample=&row;
        if (row.value("readOnly",false)) {
            const auto found=legacy_cache.find(row.at("id").get<std::string>());
            if (found==legacy_cache.end()) {++missing;continue;}
            sample=&*found;
            if (sample->value("stale",false) || sample->value("updated",std::int64_t(0))!=
                row.value("registryUpdated",row.at("updated").get<std::int64_t>()) ||
                (active(row) && now()-sample->value("observed",std::int64_t(0))>120000)) ++missing;
            else if (sample->value("tokenDay",std::int64_t(0))!=today) ++missing;
        } else if (row.contains("startCursor") && (!row.contains("usage") ||
            ((active(row) || row.value("completed",std::int64_t(0))>=today) &&
             row.value("tokenDay",std::int64_t(0))!=today))) ++missing;
        const auto tokens=sample->value("usage",Json::object());
        if (!tokens.empty()) ++sampled;
        for (auto name:usage_keys) usage[name]=usage.at(name).get<std::uint64_t>()+tokens.value(name,std::uint64_t(0));
        if (sample->value("tokenDay",std::int64_t(0))==today && sample->contains("todayTokens")) {
            total+=sample->at("todayTokens").get<std::uint64_t>();
            ++today_sampled;
        }
    }
    const auto hours=double(now()-today)/3600000.0;
    return {{"total",usage},{"todayTokens",total},{"hourlyTokens",hours>0 && (today_sampled || !missing)?
        Json(double(total)/hours):Json(nullptr)},{"missing",missing},{"sampled",sampled}};
}
Json refresh(const fs::path& data,const std::string& key,int timeout_ms=15000,bool include_history_usage=true) {
    Json settings, prior;
    {
        Database db(data, false);
        settings = db.state.at("settings");
        prior = db.state.at("sessions").at(key);
    }
    if (!prior.contains("startCursor")) return public_row(prior);
    Dsh dsh(path_from_utf8(settings.at("authPath").get<std::string>()),timeout_ms);
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
        if (const auto ending=latest_turn_end(dsh,prior.at("dshId"),start,cursor)) {
            status=ending->status;
            version=std::to_string(start)+":"+std::to_string(ending->seq);
            if (ending->time>0) completed=ending->time;
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
        else if (include_history_usage) tokens=day_usage(dsh,prior,cursor,today);
    } catch (const std::exception&) {}
    Database db(data, true);
    auto& row = db.state["sessions"].at(key);
    if (row.value("cursor",start)<=cursor && row.at("startCursor")==start &&
        (row.value("cursor",start)<cursor || row.at("status")==prior.at("status"))) {
        const auto before=row;
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
        if (row!=before) {
            if (row.at("status")!=before.at("status") || row.at("cursor")!=before.value("cursor",Json())) row["updated"]=now();
            db.save();
        }
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

Json Cluster::update_model_field(const std::string& id,const std::string& field,
                                const Json& expected,const Json& value) const {
    Database db(data_,true);
    auto& settings=db.state.at("settings");
    auto& pool=settings.at("pools").at(id);
    if (pool.at(field)==value) return settings;
    require(pool.at(field)==expected,"这项模型配置已在别处更新");
    set_model_field(pool,field,value);
    validate_configuration(settings);
    settings["revision"]=settings.at("revision").get<int>()+1;
    db.save();
    return settings;
}

namespace {
Json session_overview(const Json& settings,const Json& sessions,const LegacySessions& legacy,const fs::path& data) {
    int running=0, pending=0;
    for (const auto& row:sessions) {
        if (active(row)) ++running;
        else if (!row.contains("repairOf") && !row.value("processed",false)) ++pending;
    }
    return {{"active",running},{"unprocessed",pending},{"accounts",settings.at("accounts").size()},
        {"models",settings.at("pools").size()},{"connected",!settings.at("authPath").get<std::string>().empty()},
        {"codexSessions",legacy.rows.size()},{"codexSources",legacy.sources},{"unreadableCodexSources",legacy.unreadable},
        {"usage",telemetry_overview(sessions,legacy_usage_cache(data))}};
}
std::pair<int,int> refresh_desktop_native(const fs::path& data,DesktopMonitor& monitor,std::stop_token stop) {
    std::vector<std::pair<std::int64_t,std::string>> candidates;
    {
        Database db(data,false);
        const auto& rows=db.state.at("sessions");
        std::erase_if(monitor.native_attempts,[&](const auto& item) {return !rows.contains(item.first);});
        for (const auto& row:rows) {
            if (!active(row) || !row.contains("startCursor")) continue;
            const auto key=row.at("id").get<std::string>();
            candidates.emplace_back(monitor.native_attempts[key],key);
        }
    }
    std::sort(candidates.begin(),candidates.end());
    if (candidates.size()>24) candidates.resize(24);
    std::atomic_size_t next{};
    std::atomic_int checked{},failed{};
    const auto deadline=GetTickCount64()+3000;
    std::vector<std::jthread> workers;
    for (std::size_t i=0;i<std::min<std::size_t>(8,candidates.size());++i) {
        workers.emplace_back([&] {
            while (!stop.stop_requested() && GetTickCount64()<deadline) {
                const auto index=next.fetch_add(1);
                if (index>=candidates.size()) return;
                try {refresh(data,candidates[index].second,1100,false); ++checked;}
                catch (const std::exception&) {++failed;}
            }
        });
    }
    for (auto& worker:workers) worker.join();
    for (std::size_t i=0;i<std::min(next.load(),candidates.size());++i)
        monitor.native_attempts.at(candidates[i].second)=now();
    return {checked.load(),failed.load()};
}
}

Json Cluster::desktop_snapshot(DesktopMonitor* monitor,std::stop_token stop) const {
    Json settings,sessions;
    std::pair<int,int> native{};
    std::jthread native_worker;
    std::exception_ptr native_error;
    if (monitor) native_worker=std::jthread([&] {
        try { native=refresh_desktop_native(data_,*monitor,stop); }
        catch (...) { native_error=std::current_exception(); }
    });
    {
        Database db(data_,false);
        settings=db.state.at("settings");
        if (!monitor) sessions=db.state.at("sessions");
    }
    const auto legacy=read_legacy_sessions(settings,false,monitor?&monitor->legacy:nullptr,stop);
    if (monitor) {
        native_worker.join();
        if (native_error) std::rethrow_exception(native_error);
        Database db(data_,false);
        settings=db.state.at("settings");
        sessions=db.state.at("sessions");
    }
    sessions.update(legacy.rows);
    Json repair;
    try {
        const auto file=data_/L"handoff.json";
        if (fs::exists(file)) repair=read_json(file).value("repair",Json());
    } catch (const std::exception&) {}
    std::vector<Json> running,pending;
    for (auto row:sessions) {
        if (active(row)) running.push_back(public_row(row));
        else if (!row.contains("repairOf") && !row.value("processed",false)) {
            if (repair.is_object() && repair.at("parentId")==row.at("id") &&
                repair.at("parentVersion")==row.at("resultVersion")) {
                const auto child=sessions.find(repair.at("sessionId").get<std::string>());
                row["handoffState"]=child==sessions.end() || active(*child)?"repairing":"awaiting_acceptance";
            }
            pending.push_back(public_row(row));
        }
    }
    const auto order=[](const Json& row,const char* stamp) {
        return std::pair(row.value(stamp,std::int64_t(0)),row.at("id").get<std::string>());
    };
    std::sort(running.begin(),running.end(),[&](const Json& a,const Json& b) {return order(a,"updated")>order(b,"updated");});
    const auto pending_order=[&](const Json& row) {return std::pair(!row.contains("handoffState"),order(row,"completed"));};
    std::sort(pending.begin(),pending.end(),[&](const Json& a,const Json& b) {return pending_order(a)<pending_order(b);});
    return {{"overview",session_overview(settings,sessions,legacy,data_)},{"settings",settings},
        {"running",std::move(running)},{"pending",std::move(pending)},
        {"liveChecked",legacy.live_checked+native.first},{"liveStale",legacy.live_stale+native.second}};
}

void Cluster::refresh_legacy_usage(std::stop_token stop) const {
    const auto settings=invoke("cluster_get_settings");
    const auto legacy=read_legacy_sessions(settings,true);
    if (legacy.rows.empty()) return;
    const auto connection=discover(settings,Json::object());
    if (connection.auth.empty()) return;
    const Dsh dsh(connection.auth,2500);
    const auto day=midnight(), observed=now();
    const auto cache=legacy_usage_cache(data_);
    struct Job {
        std::string id;
        const Json* row;
        const Json* candidate;
        Json sample;
    };
    std::vector<Job> jobs;
    for (const auto& [id,row]:legacy.rows.items()) {
        const auto& candidate=legacy.usage_candidates.at(id);
        if (candidate.at("origin")!=dsh.origin()) continue;
        const auto old=cache.find(id);
        if (old!=cache.end() && !active(row) && !old->value("stale",false) &&
            old->value("observed",std::int64_t(0))>=observed-5*60*1000 &&
            old->value("updated",std::int64_t(0))==row.at("updated") &&
            old->value("tokenDay",std::int64_t(0))==day) continue;
        jobs.push_back({id,&row,&candidate,old==cache.end()?Json::object():*old});
    }
    std::sort(jobs.begin(),jobs.end(),[](const Job& a,const Job& b){return a.row->at("updated")>b.row->at("updated");});
    std::atomic_size_t next{};
    std::vector<std::jthread> fetchers;
    for (std::size_t i=0;i<std::min<std::size_t>(6,jobs.size());++i) {
        fetchers.emplace_back([&] {
            while (!stop.stop_requested()) {
                const auto index=next.fetch_add(1);
                if (index>=jobs.size()) return;
                auto& job=jobs[index];
                const auto& row=*job.row;
                const auto& candidate=*job.candidate;
                try {
                    const auto projections=dsh.request("session/projections",{{"sessionId",candidate.at("dshId")}});
                    const auto& usage=projections.at("/values/tokenUsage"_json_pointer);
                    Json totals=Json::object();
                    std::uint64_t session_tokens=0;
                    for (auto key:usage_keys) {
                        const auto& count=usage.at(key);
                        require(count.is_number_unsigned(),"DSH 用量格式无效");
                        totals[key]=count;
                        session_tokens+=count.get<std::uint64_t>();
                    }
                    job.sample["usage"]=std::move(totals);
                    job.sample["observed"]=observed;
                    job.sample["updated"]=row.at("updated");
                    std::uint64_t today_tokens=0;
                    if (candidate.value("first",std::int64_t(0))>=day) today_tokens=session_tokens;
                    else {
                        // The live cursor covers turns that resumed after a registry snapshot.
                        const auto state=dsh.request("session/state",{{"sessionId",candidate.at("dshId")}});
                        const auto cursor=state.at("cursor").get<std::int64_t>();
                        if (cursor>=0) today_tokens=day_usage(dsh,row,cursor,day);
                    }
                    job.sample["tokenDay"]=day;
                    job.sample["todayTokens"]=today_tokens;
                    job.sample["stale"]=false;
                } catch (const std::exception&) {
                    job.sample["stale"]=true;
                }
            }
        });
    }
    for (auto& thread:fetchers) thread.join();
    if (stop.stop_requested() || jobs.empty()) return;
    const ExclusiveStateLock lock(data_/L"legacy-usage.lock");
    auto updated=legacy_usage_cache(data_);
    for (auto& job:jobs) {
        if (updated.contains(job.id) && updated.at(job.id).value("observed",std::int64_t(0))>observed) continue;
        updated[job.id]=std::move(job.sample);
    }
    for (auto it=updated.begin();it!=updated.end();) {
        if (!legacy.rows.contains(it.key())) it=updated.erase(it);
        else ++it;
    }
    write_state_file(data_/L"legacy-usage.json",updated);
}

Json Cluster::start_session(const Json& input,const std::string& key,const Json& repair) const {
    const auto title = field(input,"title",400), batch = field(input,"batch",400);
    const auto role=input.contains("role")?field(input,"role",32):"developer", task=field(input,"task",64000);
    require(roles.contains(role), "未知角色");
    const auto work = path_from_utf8(field(input,"workspace",4000));
    require(work.is_absolute() && fs::is_directory(work) && work.has_relative_path(), "工作区必须是现有的绝对子目录");
    const auto workspace = fs::canonical(work);
    require(workspace.has_relative_path(), "工作区不能指向磁盘根目录");
    const auto settings=invoke("cluster_get_settings");
    require(settings.at("permission")!="","派发前需要设置并验证 DSH 权限预设");
    const auto pinned=input.contains("poolId")?field(input,"poolId",100):"";
    const auto requested=batch_allowlist(input);
    const Dsh dsh(path_from_utf8(settings.at("authPath").get<std::string>()));
    CatalogModels models;
    try {models=catalog_models(dsh.request("session/modelCatalog"));}
    catch (const std::exception&) {
        throw DispatchError("无法读取 DSH 实时模型目录",{{"reason","catalog_unavailable"},{"availablePools",Json::array()}});
    }
    LegacyObservations observations;
    const auto legacy=read_legacy_sessions(settings,false,&observations);
    Json pool,row,diagnostics;
    std::map<std::string,std::string> rejected;
    const auto reserve=[&](const std::string& cooling_account={}) {
        Database db(data_, true);
        const auto& current=db.state.at("settings");
        for (const auto name:{"authPath","configPath","permission","preset"})
            require(current.at(name)==settings.at(name),"DSH 连接设置已变化，请重新派发");
        auto& sessions = db.state["sessions"];
        if (row.empty()) require(!sessions.contains(key),"此派发请求已存在");
        auto occupied = sessions;
        occupied.erase(key);
        occupied.update(legacy.rows);
        for (const auto& value : occupied) {
            if (active(value)) require(_wcsicmp(widen(value.at("workspace").get<std::string>()).c_str(),
                workspace.c_str()) != 0, "该工作区已有活动会话");
        }
        auto& batches=db.state["batchPools"];
        auto allowed=requested;
        if (batches.contains(batch)) {
            allowed=batches.at(batch).get<std::set<std::string>>();
            if (!requested.empty() && requested!=allowed)
                throw DispatchError("此批次已有允许池清单",{{"reason","batch_allowlist_conflict"},{"allowedPoolIds",allowed}});
        }
        for (const auto& pool_id:allowed)
            if (!current.at("pools").contains(pool_id))
                throw DispatchError("批次清单包含未知池",{{"reason","unknown_pool"},{"poolId",pool_id}});
        auto& cooldowns=db.state["accountCooldowns"];
        if (cooldowns.is_null()) cooldowns=Json::object();
        if (!cooling_account.empty()) {
            cooldowns[cooling_account]=now()+60000;
            db.save();
        }
        auto choice=choose_pool(current,occupied,models,allowed,pinned,cooldowns,rejected);
        diagnostics=std::move(choice.diagnostics);
        if (choice.key.empty()) throw DispatchError(pool_failure(diagnostics,pinned),diagnostics);
        if (!allowed.empty()) batches[batch]=allowed;
        pool=current.at("pools").at(choice.key);
        if (row.empty()) row={{"id",key},{"title",title},{"role",role},{"batch",batch},{"workspace",utf8(workspace)},
            {"status","starting"},{"created",now()},{"resultVersion",""},{"read",false},{"processed",false}};
        row.update({{"poolId",choice.key},{"provider",pool.at("provider")},{"model",pool.at("model")},{"updated",now()}});
        if (!repair.is_null()) row["repairOf"]=repair;
        sessions[key] = row;
        db.save();
    };
    reserve();
    bool may_have_dispatched = false;
    std::string stage="create_session";
    try {
        Json create{{"cwd",utf8(workspace)}};
        if (settings.at("preset") != "") create["agentPreset"] = settings.at("preset");
        const auto created = dsh.request("session/create", create);
        const auto sid = field(created, "sessionId", 200);
        row["dshId"] = sid;
        row["owner"] = dsh.owner();
        if (created.contains("agentPreset") && settings.at("preset") != "")
            require(created.at("agentPreset") == settings.at("preset"), "DSH 未保留 Agent 预设");
        stage="permission_preset";
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
        stage="select_model";
        for (;;) {
            const auto provider=pool.at("provider").get<std::string>(),model=pool.at("model").get<std::string>();
            const auto reasoning=models.at({provider,model}).value("reasoning",Json::object());
            const auto efforts=reasoning.is_object()?reasoning.value("efforts",Json::array()):Json::array();
            const auto supports=[&](const std::string& value) {
                return !value.empty() && std::ranges::any_of(efforts,[&](const Json& item) {
                    return item.is_object() && item.value("id","")==value;
                });
            };
            auto effort=pool.at("effort").get<std::string>();
            if (!supports(effort)) effort=reasoning.is_object()?reasoning.value("defaultEffort",""):"";
            if (!supports(effort)) effort.clear();
            Json select{{"sessionId",sid},{"provider",provider},{"model",model}};
            if (!effort.empty()) select["reasoningEffort"]=effort;
            try {
                const auto result=dsh.request("session/selectModel",select);
                const auto& selected=result.at("selected");
                require(selected.at("provider")==provider && selected.at("model")==model &&
                    (effort.empty() || selected.value("reasoningEffort","")==effort),"DSH 所选模型或思考强度与配置不符");
                row["effort"]=effort;
                break;
            } catch (const DshError& error) {
                const auto reason=selection_rejection(error);
                if (reason.empty()) throw;
                rejected[row.at("poolId").get<std::string>()]=reason;
                reserve(reason=="account_cooldown"?provider:"");
            }
        }
        stage="session_state";
        const auto live = rpc_state(dsh, row);
        row["startCursor"] = live.at("cursor");
        row["cursor"] = live.at("cursor");
        {
            Database db(data_, true);
            db.state["sessions"][key] = row;
            db.save();
        }
        stage="submit_prompt";
        may_have_dispatched = true;
        const auto prompt = "Work in the assigned workspace only. Do not change provider/model, start nested agents, "
            "or publish changes without authorization. Return verifiable evidence. In the final answer include a fenced "
            "```fatfish-delivery JSON block with candidateSha (full commit hash or null), worktreeClean (boolean), "
            "tests (array of {command,exitCode}), and evidencePaths (array of local absolute paths). "
            "Only report tests you actually ran.\n\n" + task;
        const auto reply = dsh.request("session/prompt", {{"sessionId",sid},{"requestId",key},{"mode","queue"},
            {"content",Json::array({{{"type","text"},{"text",prompt}}})}});
        may_have_dispatched = reply.value("accepted",true);
        require(may_have_dispatched, "DSH 拒绝了任务");
        row["status"] = "queued";
        if (!rejected.empty()) row["poolFallbacks"]=rejected;
    } catch (const std::exception& e) {
        row["status"] = may_have_dispatched ? "unknown" : "failed";
        row["error"] = e.what();
        row["dispatch"]={{"stage",stage},{"diagnostics",diagnostics},{"mayHaveDispatched",may_have_dispatched}};
        if (const auto* failure=dynamic_cast<const DshError*>(&e)) {
            const auto reason=selection_rejection(*failure);
            if (!reason.empty()) row["dispatch"]["reason"]=reason;
        }
        if (!may_have_dispatched) {
            row["resultVersion"] = "dispatch:failed";
            row["completed"] = now();
            row.erase("startCursor");
        }
    }
    Database db(data_, true);
    auto& stored = db.state["sessions"].at(key);
    if (stored.at("status")=="starting" || row.at("status")=="failed" ||
        (stored.at("status")=="queued" && row.at("status")=="unknown" &&
         stored.value("cursor",Json())==row.value("cursor",Json()))) {
        stored = row;
        db.save();
    } else if (row.contains("poolFallbacks")) {
        stored["poolFallbacks"]=row.at("poolFallbacks");
        db.save();
    }
    return public_row(stored);
}

Json Cluster::invoke(std::string_view tool, const Json& input) const {
    require(input.is_object(), "工具参数必须为 JSON 对象");
    if (tool == "cluster_overview" || tool == "cluster_get_settings") {
        Json settings,sessions;
        {
            Database db(data_,false);
            settings=db.state.at("settings");
            if (tool=="cluster_get_settings") return settings;
            sessions=db.state.at("sessions");
        }
        LegacyObservations observations;
        const auto legacy=read_legacy_sessions(settings,false,&observations);
        sessions.update(legacy.rows);
        return session_overview(settings,sessions,legacy,data_);
    }
    if (tool=="cluster_repair_handoff") {
        const auto token=field(input,"claimToken",100),version=field(input,"originalVersion",100);
        const auto request=field(input,"requestId",100),sha=field(input,"candidateSha",64);
        require(commit_sha(sha),"候选 SHA 必须是完整的 Git 提交哈希");
        const auto paths=input.value("screenshotPaths",Json::array());
        require(evidence_paths(paths),"请提供有效的截图绝对路径");
        const auto acceptance=field(input,"acceptance",8000);
        const auto task=field(input,"task",50000);
        const auto queue_file=data_/L"handoff.json";
        const ExclusiveStateLock lock(data_/L"handoff.lock");
        auto queue=fs::exists(queue_file)?read_json(queue_file):Json::object();
        const auto claim=queue.value("claim",Json::object());
        require(claim.value("claimToken","")==token && claim.value("expiresAt",std::int64_t(0))>now(),
            "需要当前队首的有效领取凭据");
        require(claim.at("resultVersion")==version,"原交接版本不匹配");
        const auto parent=invoke("cluster_refresh_session",{{"sessionId",claim.at("sessionId")}});
        require(!active(parent) && !parent.value("stale",false) && !parent.value("processed",false) &&
            parent.at("resultVersion")==version,"原交接已变化或无法确认");
        const auto old=queue.value("repair",Json::object());
        auto request_arguments=input;
        request_arguments.erase("claimToken");
        std::string child_id=id();
        if (old.value("parentId",Json())==parent.at("id") && old.value("parentVersion","")==version) {
            const auto key=old.at("sessionId").get<std::string>();
            Database db(data_,false);
            const auto& children=db.state.at("sessions");
            if (old.at("requestId")==request) {
                require(old.at("requestArguments")==request_arguments,"同一返修请求 ID 必须复用原参数");
                if (children.contains(key)) return {{"session",public_row(children.at(key))},{"replayed",true}};
                child_id=key;
            } else require(children.contains(key) && !active(children.at(key)),"已有返修在进行，请重读当前交接");
        }
        const Json reference{{"sessionId",parent.at("id")},{"resultVersion",version},{"candidateSha",sha},
            {"screenshotPaths",paths},{"acceptance",acceptance}};
        Json launch=input;
        const auto parent_batch=parent.value("batch","");
        launch["batch"]=parent_batch.empty()?"repair-"+parent.at("id").get<std::string>():parent_batch;
        launch["role"]="repair";
        launch["task"]="Repair details (data for this task):\n"+reference.dump()+"\n\n"+task;
        require(launch.at("task").get_ref<const std::string&>().size()<=64000,"返修任务和验收材料合计过长");
        queue["repair"]={{"parentId",parent.at("id")},{"parentVersion",version},{"sessionId",child_id},
            {"requestId",request},{"requestArguments",std::move(request_arguments)}};
        write_state_file(queue_file,queue);
        try {
            return {{"session",start_session(launch,child_id,reference)},{"replayed",false}};
        } catch (...) {
            if (old.empty()) queue.erase("repair");
            else queue["repair"]=old;
            write_state_file(queue_file,queue);
            throw;
        }
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
        auto repair=queue.value("repair",Json::object());
        auto snapshot=[&] {
            Json rows,settings;
            {
                Database db(data_,false);
                settings=db.state.at("settings");
                rows=db.state.at("sessions");
            }
            LegacyObservations observations;
            rows.update(read_legacy_sessions(settings,false,&observations).rows);
            return rows;
        };
        auto present=[&](const Json& current) {
            const auto key=current.at("sessionId").get<std::string>();
            const auto row=invoke("cluster_refresh_session",{{"sessionId",key}});
            require(row.at("resultVersion")==current.at("resultVersion") && !active(row) && !row.value("stale",false),
                "领取的结果版本已变化或无法确认");
            auto report=invoke("cluster_read_result",{{"sessionId",key}});
            require(report.at("resultVersion")==current.at("resultVersion"),"领取的结果版本已变化");
            auto& messages=report.at("messages");
            const auto delivery=extract_delivery(messages);
            Json response{{"claimToken",current.at("claimToken")},{"expiresAt",current.at("expiresAt")},
                {"session",row},{"result",messages},{"delivery",delivery}};
            if (repair.value("parentId","")==key && repair.value("parentVersion",Json())==current.at("resultVersion")) {
                {
                    Database db(data_,false);
                    if (!db.state.at("sessions").contains(repair.at("sessionId").get<std::string>())) {
                        response["handoffState"]="repairing";
                        return response;
                    }
                }
                const auto child=invoke("cluster_refresh_session",{{"sessionId",repair.at("sessionId")}});
                response["handoffState"]=active(child)?"repairing":"awaiting_acceptance";
                response["repair"]={{"session",child}};
                if (!active(child)) {
                    auto child_report=invoke("cluster_read_result",{{"sessionId",child.at("id")}});
                    auto& child_messages=child_report.at("messages");
                    response["repair"]["delivery"]=extract_delivery(child_messages);
                    response["repair"]["result"]=std::move(child_messages);
                }
            }
            return response;
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
            const auto current=invoke("cluster_refresh_session",{{"sessionId",key}});
            require(!current.value("stale",false),"暂时无法确认已领取的结果，请重试");
            if (current.at("resultVersion")!=version || active(current)) {
                queue.erase("claim");
                if (repair.value("parentId","")==key) queue.erase("repair");
                write_state_file(queue_file,queue);
                return {{"changed",true}};
            }
            if (repair.value("parentId","")==key && repair.value("parentVersion","")==version) {
                const auto child=invoke("cluster_refresh_session",{{"sessionId",repair.at("sessionId")}});
                require(!active(child),"返修仍在进行，完成主代理验收后才能确认交接");
                queue.erase("repair");
                repair=Json::object();
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

        auto rows=snapshot();
        if (!repair.empty()) {
            const auto parent=rows.find(repair.at("parentId").get<std::string>());
            if (parent==rows.end()) return {{"busy",true},{"handoffState","pending_sync"}};
            if (parent->value("stale",false)) {
                *parent=invoke("cluster_refresh_session",{{"sessionId",parent->at("id")}});
                if (parent->value("stale",false)) return {{"busy",true},{"handoffState","pending_sync"}};
            }
            if (active(*parent) || parent->value("processed",false) || parent->at("resultVersion")!=repair.at("parentVersion")) {
                queue.erase("repair");
                repair=Json::object();
                write_state_file(queue_file,queue);
            }
        }
        std::vector<Json> pending;
        const auto sort_key=[](const Json& row) {
            return std::pair(row.value("completed",std::int64_t(0)),row.at("id").get<std::string>());
        };
        for (auto& row:rows) {
            if (active(row) || row.contains("repairOf") || row.value("processed",false) || row.value("resultVersion","").empty()) continue;
            pending.push_back(std::move(row));
        }
        const auto priority=[&](const Json& row) {
            return std::pair(!(repair.value("parentId",Json())==row.at("id") &&
                repair.value("parentVersion",Json())==row.at("resultVersion")),sort_key(row));
        };
        std::ranges::sort(pending,[&](const Json& a,const Json& b){return priority(a)<priority(b);});
        for (const auto& candidate:pending) {
            auto row=candidate;
            if (row.value("source","")=="codex") {
                row=invoke("cluster_refresh_session",{{"sessionId",candidate.at("id")}});
                if (row.value("stale",false) || active(row) || row.value("processed",false) ||
                    row.at("resultVersion")!=candidate.at("resultVersion")) {
                    if (repair.value("parentId",Json())==candidate.at("id")) return {{"busy",true},{"handoffState","pending_sync"}};
                    continue;
                }
            }
            claim={{"source",row.value("source","fatfishbox")},{"sessionId",row.at("id")},
                {"resultVersion",row.at("resultVersion")},{"claimToken",id()},
                {"expiresAt",now()+30*60*1000}};
            auto result=present(claim);
            queue["claim"]=claim;
            if (!token.empty()) queue["nextToken"]=claim.at("claimToken");
            write_state_file(queue_file,queue);
            result["completed"]=!token.empty();
            return result;
        }
        return {{"empty",true},{"completed",!token.empty()}};
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
    if (tool == "cluster_configure" || tool == "cluster_update_pool") {
        Database db(data_, true);
        auto& settings = db.state["settings"];
        require(settings.at("revision") == input.at("revision"), "配置已变化，请刷新后重试");
        if (tool == "cluster_configure") {
            for (const auto member : {"configPath","authPath","permission","preset"})
                if (input.contains(member)) settings[member] = input.at(member);
        } else {
            auto& pool = settings.at("pools").at(field(input, "id", 512));
            for (const auto member : {"enabled","concurrency","effort"})
                if (input.contains(member)) set_model_field(pool,member,input.at(member));
        }
        validate_configuration(settings);
        settings["revision"] = settings["revision"].get<int>() + 1;
        db.save();
        return settings;
    }
    if (tool == "cluster_start_session") return start_session(input,id());
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
        const auto settings=invoke("cluster_get_settings");
        if (tool == "cluster_read_result") return legacy_result(settings,key);
        if (tool == "cluster_refresh_session") return live_legacy_session(settings,key);
        if (tool == "cluster_get_session")
            return read_legacy_sessions(settings).rows.at(key);
        throw std::runtime_error("此会话属于原 Codex 插件，请在原插件中取消或确认");
    }
    if (tool == "cluster_refresh_session") return refresh(data_,key);
    if (tool == "cluster_mark_receipt") {
        const ExclusiveStateLock lock(data_/L"handoff.lock");
        const auto queue_file=data_/L"handoff.json";
        if (fs::exists(queue_file)) {
            const auto repair=read_json(queue_file).value("repair",Json::object());
            require(repair.value("parentId","")!=key,"请使用当前领取凭据完成返修验收和确认");
        }
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
    if (tool=="cluster_read_result" && row.value("resultVersion","")=="dispatch:failed")
        return {{"sessionId",key},{"resultVersion","dispatch:failed"},{"messages",Json::array({
            {{"type","dispatch/error"},{"text",row.value("error","派发失败")}}})}};
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
            if (type!="assistant/message" && type!="assistant/response" && type!="user/message") continue;
            const auto message_path="/data/message/content"_json_pointer;
            const auto direct_path="/data/content"_json_pointer;
            const auto path=event.contains(message_path)?message_path:direct_path;
            if (!event.contains(path)) continue;
            const auto& content=event.at(path);
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
        pool_list{{"type","array"},{"items",text},{"minItems",1},{"maxItems",64}},
        path_list{{"type","array"},{"items",text},{"maxItems",16}};
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
    add("cluster_configure","更新 DSH 连接与预设。",
        {{"revision",integer},{"configPath",text},{"authPath",text},{"permission",text},{"preset",text}}, {"revision"});
    add("cluster_update_pool","设置模型启用状态、总并发和思考强度。",
        {{"revision",integer},{"id",text},{"enabled",boolean},{"concurrency",integer},{"effort",text}}, {"revision","id"});
    add("cluster_next_handoff","获取下一个需处理对话。完成实际处置后，传回 claimToken 和 outcome 自动确认并领取下一条。省略 outcome 可用领取凭据重读本条。",
        {{"claimToken",text},{"outcome",text}});
    tools.back()["title"]="获取下一个需处理对话";
    add("cluster_start_session","在授权工作区派发会话。allowedPoolIds 为批次允许池清单，首次设置后同批次沿用。poolId 固定单池，省略时可在范围内自动换池。",
        {{"title",text},{"batch",text},{"role",text},{"workspace",text},{"task",text},{"poolId",text},{"allowedPoolIds",pool_list}},
        {"title","batch","workspace","task"});
    add("cluster_repair_handoff","从当前领取的交接派发返修。返修结果挂在原队首，主代理验收后确认。重试同一请求时复用 requestId。",
        {{"claimToken",text},{"originalVersion",text},{"requestId",text},{"candidateSha",text},{"screenshotPaths",path_list},
         {"acceptance",text},{"title",text},{"workspace",text},{"task",text},{"poolId",text},{"allowedPoolIds",pool_list}},
        {"claimToken","originalVersion","requestId","candidateSha","acceptance","title","workspace","task"});
    add("cluster_refresh_sessions","同步本地登记的最早活动会话，每次最多 20 个。",{{"limit",integer}});
    add("cluster_cancel_session","取消已知的活动会话。",{{"sessionId",text}},{"sessionId"});
    return tools;
}
}
