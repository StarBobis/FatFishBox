#include "legacy.h"
#include "dsh.h"
#include "state_file.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace fatfish {
namespace {
constexpr std::size_t max_registry = 64 * 1024 * 1024;

fs::path env_path(const wchar_t* name) {
    auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), length));
    return fs::path(value);
}

std::string text(const Json& object, const char* field, std::string otherwise = {}) {
    const auto found = object.find(field);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::move(otherwise);
}

std::int64_t milliseconds(const Json& row, const char* field) {
    return row.contains(field) && row.at(field).is_number() ? std::int64_t(row.at(field).get<double>() * 1000) : 0;
}

bool terminal(const std::string& status) {
    return status == "succeeded" || status == "failed" || status == "cancelled" ||
           status == "interrupted" || status == "policy_violation" || status == "tool_arguments_error";
}

std::set<fs::path> registries(const Json& settings) {
    const auto override = env_path(L"FATFISHBOX_LEGACY_STATE_DIR");
    auto fallback = env_path(L"DSH_WORKERS_STATE");
    if (fallback.empty()) fallback = env_path(L"LOCALAPPDATA") / L"dsh-workers";
    if (!override.empty()) return override.is_absolute() ? std::set{override / L"registry/workers.json"} : std::set<fs::path>{};
    std::set<fs::path> result;
    if (fallback.is_absolute()) result.insert(fallback / L"registry/workers.json");
    std::set<fs::path> configs;
    const auto configured = path_from_utf8(text(settings,"configPath"));
    if (configured.is_absolute()) configs.insert(configured);
    const auto primary = env_path(L"DSH_WORKERS_CONFIG");
    if (primary.is_absolute()) configs.insert(primary);
    const auto home = env_path(L"USERPROFILE");
    const auto profiles = home / L".deepseek-workers";
    std::error_code error;
    for (fs::directory_iterator it(profiles,error); !error && it != fs::directory_iterator(); it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && it->path().extension() == L".json") configs.insert(it->path());
    }
    for (const auto& config : configs) {
        try {
            const auto root = path_from_utf8(text(read_json(config),"stateDir"));
            if (!root.empty()) result.insert((config.parent_path() / root).lexically_normal() / L"registry/workers.json");
        } catch (const std::exception&) {}
    }
    return result;
}

struct Entry {
    Json record;
    Json mark;
    std::string id;
    fs::path review_path, registry;
    std::string review_key;
};

std::map<std::string,Entry> load(const Json& settings, int& sources, int& unreadable) {
    std::map<std::string,Entry> records;
    for (const auto& registry : registries(settings)) {
        std::error_code error;
        if (!fs::exists(registry,error) && !error) continue;
        Json data, marks = Json::object();
        try {
            data = read_json(registry,max_registry);
            if (!data.at("sessions").is_object()) throw std::runtime_error("会话登记格式无效");
            ++sources;
        } catch (const std::exception&) {
            ++unreadable;
            continue;
        }
        const auto mark_path = registry.parent_path() / L"monitor-review.json";
        try {
            if (fs::exists(mark_path)) marks = read_json(mark_path,max_registry);
            if (!marks.is_object()) throw std::runtime_error("回执格式无效");
        } catch (const std::exception&) {
            ++unreadable;
            marks=Json::object();
        }
        for (const auto& [key,row] : data.at("sessions").items()) {
            const auto sid = text(row,"sessionId");
            const auto status = text(row,"status");
            if (sid.empty() || status.empty()) continue;
            const auto origin = text(row,"serviceOrigin");
            const Json owner = origin.empty() ? Json::array({utf8(registry),row.value("serviceId",Json(nullptr))}) : Json(origin);
            const auto ref = Json::array({owner,sid});
            Json matched;
            // Python formats the JSON keys with a space after each delimiter.
            const auto owner_key = owner.is_array() ? "[" + owner[0].dump() + ", " + owner[1].dump() + "]" : owner.dump();
            const auto spaced = "[" + owner_key + ", " + Json(sid).dump() + "]";
            if (marks.contains(spaced)) matched = marks.at(spaced);
            else if (marks.contains(ref.dump())) matched = marks.at(ref.dump());
            const auto local = "codex:" + sid;
            auto existing = records.find(local);
            if (existing == records.end() || milliseconds(row,"updatedAt") > milliseconds(existing->second.record,"updatedAt"))
                records.insert_or_assign(local,Entry{row,matched,local,mark_path,registry,
                    marks.contains(ref.dump()) && !marks.contains(spaced)?ref.dump():spaced});
        }
    }
    return records;
}

Json projection(const Entry& entry) {
    const auto& row = entry.record;
    auto status = text(row,"status");
    auto start = row.value("startSeq",Json(-1));
    auto end = row.value("cursor",Json(nullptr));
    std::string version;
    if (terminal(status) && end.is_number_integer()) {
        version = (start.is_number_integer()?start.dump():"-1") + ":" + end.dump();
    }
    const auto exact = !version.empty() && entry.mark.is_object() && text(entry.mark,"version") == version;
    const auto read = exact && entry.mark.value("read",false);
    const auto processed = exact && entry.mark.value("processed",false);
    if (status == "policy_violation" || status == "tool_arguments_error") status = "failed";
    const auto updated = milliseconds(row,"updatedAt");
    const auto ended = milliseconds(row,"endedAt");
    return {{"id",entry.id},{"source","codex"},{"readOnly",true},{"title",text(row,"title",text(row,"sessionId"))},
        {"dshId",text(row,"sessionId")},{"batch",text(row,"batchId")},{"workspace",text(row,"workspace")},
        {"role",text(row,"role")},{"provider",text(row,"provider")},{"model",text(row,"model")},
        {"status",status},{"updated",updated},{"completed",version.empty()?0:(ended?ended:updated)},
        {"resultVersion",version},{"read",read},{"processed",processed}};
}
}

LegacySessions read_legacy_sessions(const Json& settings) {
    LegacySessions result;
    for (const auto& [key,entry] : load(settings,result.sources,result.unreadable)) {
        result.rows[key] = projection(entry);
    }
    return result;
}

Json legacy_result(const Json& settings, const std::string& id) {
    int sources=0, unreadable=0;
    const auto entries=load(settings,sources,unreadable);
    const auto found=entries.find(id);
    if (found==entries.end()) throw std::runtime_error("原插件登记中没有该会话");
    const auto row=projection(found->second);
    std::string output=text(found->second.record,"assistantText");
    const bool clipped=output.size()>6000;
    if (clipped) {
        auto start=output.size()-6000;
        while (start<output.size() && (static_cast<unsigned char>(output[start])&0xc0)==0x80) ++start;
        output.erase(0,start);
    }
    Json messages=Json::array();
    if (!output.empty()) messages.push_back({{"type","assistant/message"},{"text",output},{"truncated",clipped}});
    return {{"sessionId",id},{"source","codex"},{"readOnly",true},{"resultVersion",row.at("resultVersion")},{"messages",messages}};
}

void complete_legacy_handoff(const Json& settings,const std::string& id,const std::string& version,
                             const std::string& outcome) {
    int sources=0, unreadable=0;
    const auto entries=load(settings,sources,unreadable);
    const auto& entry=entries.at(id);
    auto lock_path=entry.review_path;
    lock_path.replace_extension(L".lock");
    const ExclusiveStateLock lock(lock_path);
    const auto registry=read_json(entry.registry,max_registry);
    const auto& sessions=registry.at("sessions");
    const auto record=std::ranges::find_if(sessions,[&](const Json& row) {
        return text(row,"sessionId")==text(entry.record,"sessionId") &&
            text(row,"serviceOrigin")==text(entry.record,"serviceOrigin");
    });
    if (record==sessions.end() || projection(Entry{*record,{},id,{}, {}, {}}).at("resultVersion")!=version)
        throw std::runtime_error("Codex 会话的结果版本已变化");
    auto marks=fs::exists(entry.review_path)?read_json(entry.review_path,max_registry):Json::object();
    if (!marks.is_object()) throw std::runtime_error("Codex 回执格式无效");
    auto& mark=marks[entry.review_key];
    if (mark.is_object() && mark.value("version","")==version && mark.value("processed",false)) return;
    mark={{"version",version},{"read",true},{"processed",true},{"outcome",outcome}};
    write_state_file(entry.review_path,marks);
}
}
