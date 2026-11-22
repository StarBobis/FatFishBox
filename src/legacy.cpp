#include "legacy.h"
#include "dsh_history.h"
#include "dsh.h"
#include "state_file.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

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

std::int64_t sequence(const Json& row,const char* field,std::int64_t fallback=-1) {
    const auto value=row.find(field);
    return value!=row.end() && value->is_number_integer()?value->get<std::int64_t>():fallback;
}

bool terminal(const std::string& status) {
    return status == "succeeded" || status == "failed" || status == "cancelled" ||
           status == "interrupted" || status == "policy_violation" || status == "tool_arguments_error";
}

using RegistryRoutes = std::map<fs::path,std::set<fs::path>>;

RegistryRoutes registries(const Json& settings) {
    const auto override = env_path(L"FATFISHBOX_LEGACY_STATE_DIR");
    auto fallback = env_path(L"DSH_WORKERS_STATE");
    if (fallback.empty()) fallback = env_path(L"LOCALAPPDATA") / L"dsh-workers";
    else if (fallback.is_relative()) fallback=fs::absolute(fallback);
    RegistryRoutes result;
    std::map<fs::path,std::set<fs::path>> configured_routes;
    if (!override.empty() && override.is_relative()) return result;
    const auto default_registry=(override.empty()?fallback:override).lexically_normal()/L"registry/workers.json";
    if (default_registry.is_absolute()) {
        result[default_registry];
    }
    std::set<fs::path> configs;
    const auto add_profile=[&](const Json& profile) {
        const auto config=path_from_utf8(text(profile,"configPath"));
        const auto auth=path_from_utf8(text(profile,"authPath"));
        if (config.is_absolute()) {
            configs.insert(config);
            if (auth.is_absolute()) configured_routes[config].insert(auth);
        }
        if (default_registry.is_absolute() && auth.is_absolute()) result[default_registry].insert(auth);
    };
    add_profile(settings);
    if (settings.contains("dshConnections"))
        for (const auto& profile:settings.at("dshConnections")) add_profile(profile);
    const auto primary = env_path(L"DSH_WORKERS_CONFIG");
    if (primary.is_absolute()) configs.insert(primary);
    const auto home = env_path(L"USERPROFILE");
    const auto profiles = home / L".deepseek-workers";
    std::error_code error;
    if (override.empty()) {
        for (fs::directory_iterator it(profiles,error); !error && it != fs::directory_iterator(); it.increment(error)) {
            std::error_code entry_error;
            if (it->is_regular_file(entry_error) && it->path().extension() == L".json") configs.insert(it->path());
        }
    }
    for (const auto& config : configs) {
        try {
            const auto raw=read_json(config);
            const auto root=config_relative_path(config,text(raw,"stateDir"));
            const auto registry=override.empty() && !root.empty()?root/L"registry/workers.json":default_registry;
            auto& routes=result[registry];
            if (const auto found=configured_routes.find(config);found!=configured_routes.end())
                routes.insert(found->second.begin(),found->second.end());
            const auto service=raw.find("service");
            if (service==raw.end() || !service->is_object()) continue;
            const auto attached=config_relative_path(config,text(*service,"attachAuthFile"));
            if (attached.is_absolute()) routes.insert(attached);
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
    std::set<fs::path> attached_auth;
};

std::map<std::string,Entry> load(const Json& settings, int& sources, int& unreadable) {
    std::map<std::string,Entry> records;
    for (const auto& [registry,attached] : registries(settings)) {
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
            if (!row.is_object()) continue;
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
            auto existing = records.find(spaced);
            if (existing == records.end() || milliseconds(row,"updatedAt") > milliseconds(existing->second.record,"updatedAt")) {
                records.insert_or_assign(spaced,Entry{row,matched,local,mark_path,registry,
                    marks.contains(ref.dump()) && !marks.contains(spaced)?ref.dump():spaced,attached});
            } else existing->second.attached_auth.insert(attached.begin(),attached.end());
        }
    }
    std::map<std::string,Entry> by_id;
    for (auto& [identity,entry]:records) {
        const auto existing=by_id.find(entry.id);
        if (existing==by_id.end()) {
            const auto id=entry.id;
            by_id.emplace(id,std::move(entry));
        } else {
            // Preserve the old ID for the first registry identity sharing a session ID.
            std::uint64_t hash=14695981039346656037ull;
            for (const auto ch:identity) hash=(hash^static_cast<unsigned char>(ch))*1099511628211ull;
            entry.id+="@"+std::to_string(hash);
            const auto id=entry.id;
            by_id.emplace(id,std::move(entry));
        }
    }
    return by_id;
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
    Json failure=Json::object();
    if (status=="failed" || status=="interrupted" || status=="policy_violation" || status=="tool_arguments_error") {
        auto kind=text(row,"failureKind");
        if (kind.empty()) kind=status=="failed"?"error":status;
        failure=failure_details({{"kind",kind},{"error",row.value("error",Json())}});
        const auto reasons=row.value("policyReasons",Json::array());
        if (reasons.is_array() && !reasons.empty()) failure["policyReasons"]=reasons;
    }
    if (status == "policy_violation" || status == "tool_arguments_error") status = "failed";
    const auto updated = milliseconds(row,"updatedAt");
    const auto ended = milliseconds(row,"endedAt");
    auto effort=text(row,"reasoningEffort");
    const auto observed=row.find("actualReasoningEfforts");
    if (observed!=row.end() && observed->is_array()) {
        for (auto item=observed->rbegin();item!=observed->rend();++item) {
            if (!item->is_string() || item->get_ref<const std::string&>().empty()) continue;
            effort=item->get<std::string>();
            break;
        }
    }
    return {{"id",entry.id},{"source","codex"},{"readOnly",true},{"title",text(row,"title",text(row,"sessionId"))},
        {"dshId",text(row,"sessionId")},{"batch",text(row,"batchId")},{"workspace",text(row,"workspace")},
        {"role",text(row,"role")},{"provider",text(row,"provider")},{"model",text(row,"model")},{"effort",effort},
        {"status",status},{"updated",updated},{"completed",version.empty()?0:(ended?ended:updated)},
        {"resultVersion",version},{"read",read},{"processed",processed},{"failure",std::move(failure)}};
}

fs::path archive_file(const Entry& entry) {
    if (!terminal(text(entry.record,"status"))) return {};
    return dsh_history_file(text(entry.record,"sessionId"),text(entry.record,"workspace"));
}

std::optional<DshHistory> archived_turn(const Entry& entry,const fs::path& file) {
    const auto history=read_dsh_history(file,text(entry.record,"sessionId"),sequence(entry.record,"startSeq"));
    if (!history || history->cursor<sequence(entry.record,"cursor")) return {};
    return history;
}

Json archived_result(const Entry& entry,const DshHistory& turn) {
    auto row=projection(entry);
    const auto version=std::to_string(sequence(entry.record,"startSeq"))+":"+std::to_string(turn.ending.seq);
    auto failure=row.at("resultVersion")==version?row.at("failure"):Json::object();
    failure.update(turn.ending.failure);
    return {{"status",turn.ending.status},{"resultVersion",version},{"resultSource","dsh_history"},
        {"completed",turn.ending.time>0?turn.ending.time:row.at("completed").get<std::int64_t>()},
        {"updated",row.at("updated")},{"historyCursor",turn.cursor},{"failure",std::move(failure)}};
}

void apply_live(Json& row,const Entry& entry,const LegacyObservation& observation) {
    row["stale"]=observation.stale;
    if (observation.live.empty()) return;
    row["registryUpdated"]=row.at("updated");
    row.update(observation.live);
    const auto version=text(row,"resultVersion");
    const bool exact=!version.empty() && entry.mark.is_object() && text(entry.mark,"version")==version;
    row["read"]=exact && entry.mark.value("read",false);
    row["processed"]=exact && entry.mark.value("processed",false);
}

LegacyObservation observe(const Entry& entry,const Dsh& dsh,const LegacyObservation& previous) {
    auto result=previous;
    result.checked=now();
    result.stale=false;
    const auto& record=entry.record;
    const auto start=sequence(record,"startSeq");
    const auto live=dsh.request("session/state",{{"sessionId",record.at("sessionId")}});
    if (!live.at("cursor").is_number_integer() || !live.at("running").is_boolean())
        throw std::runtime_error("DSH 会话状态格式无效");
    const auto cursor=sequence(live,"cursor");
    const auto registered=text(record,"status");
    auto row=projection(entry);
    std::string status,version;
    Json failure=Json::object();
    std::int64_t completed=0;
    if (!live.value("pendingInput",Json()).empty()) status="waiting_input";
    else if (live.at("running").get<bool>()) status="running";
    else if (has_queued_prompt(live)) status="queued";
    else if (cursor==sequence(previous.live,"liveCursor") && terminal(text(previous.live,"status"))) {
        status=text(previous.live,"status");
        version=text(previous.live,"resultVersion");
        completed=previous.live.value("completed",std::int64_t(0));
        failure=previous.live.value("failure",Json::object());
    } else if (registered=="policy_violation" && cursor==sequence(record,"cursor")) {
        status="failed";
        version=text(row,"resultVersion");
        completed=row.at("completed").get<std::int64_t>();
    } else if (const auto ending=latest_turn_end(dsh,record.at("sessionId"),start,cursor)) {
        status=ending->status;
        version=std::to_string(start)+":"+std::to_string(ending->seq);
        completed=ending->time>0?ending->time:row.value("completed",std::int64_t(0));
        if (completed<=0) completed=result.checked;
        failure=ending->failure;
    } else if (terminal(registered) && cursor==sequence(record,"cursor")) {
        status=text(row,"status");
        version=text(row,"resultVersion");
        completed=row.at("completed").get<std::int64_t>();
    } else status=registered=="starting" && cursor<=start?"starting":"unknown";
    if (registered=="cancelling_policy_violation" &&
        (status=="running" || status=="queued" || status=="waiting_input")) status=registered;
    const auto updated=cursor==sequence(previous.live,"liveCursor") && status==text(previous.live,"status")?
        previous.live.value("updated",std::int64_t(0)):
        (cursor==sequence(record,"cursor") && status==text(row,"status")?row.at("updated").get<std::int64_t>():
         std::max(completed,result.checked));
    if ((status=="failed" || status=="interrupted") && !version.empty() && row.at("resultVersion")==version) {
        auto registered_failure=row.at("failure");
        registered_failure.update(failure);
        failure=std::move(registered_failure);
    }
    result.live={{"status",status},{"resultVersion",version},{"completed",completed},{"updated",updated},{"liveCursor",cursor},
        {"failure",std::move(failure)}};
    return result;
}

using Auths = std::map<fs::path,std::shared_ptr<Dsh>>;

std::shared_ptr<Dsh> route(const Entry& entry,Auths& auths) {
    const auto service=text(entry.record,"serviceId");
    const auto origin=text(entry.record,"serviceOrigin");
    auto paths=entry.attached_auth;
    if (service!="attached" && service.starts_with("svc-") &&
        service.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")==std::string::npos) {
        const auto root=entry.registry.parent_path().parent_path();
        paths={root/L"services"/path_from_utf8(service)/L"auth-private.json"};
    } else if (service!="attached" && origin.empty()) return {};
    for (const auto& path:paths) {
        if (const auto [pos,fresh]=auths.try_emplace(path);fresh) {
            try { pos->second=std::make_shared<Dsh>(path,1100); }
            catch (const std::exception&) {}
        }
        const auto& dsh=auths.at(path);
        if (dsh && (origin.empty() || origin==dsh->origin() || origin==dsh->origin()+"/" ||
                    (service=="attached" && dsh->matches_desktop_origin(origin)))) return dsh;
    }
    return {};
}

Json live_projection(const Entry& entry,const Dsh* dsh) {
    auto row=projection(entry);
    if (dsh) {
        try { apply_live(row,entry,observe(entry,*dsh,{})); }
        catch (const std::exception&) {row["stale"]=true;}
    } else if (!text(entry.record,"serviceId").empty()) row["stale"]=true;
    return row;
}

std::string live_output(const Entry& entry,const Dsh& dsh,const Json& row) {
    const auto start=sequence(entry.record,"startSeq");
    const auto cursor=sequence(row,"liveCursor");
    Json input{{"address",{{"kind","session"},{"sessionId",entry.record.at("sessionId")}}},
        {"throughSeq",cursor},{"maxMessages",4}};
    for (int i=0;i<4;++i) {
        Json page;
        for (;;) {
            try {page=dsh.request("session/page",input);break;}
            catch (const std::length_error&) {
                const auto size=input.at("maxMessages").get<int>();
                if (size<=1) throw;
                input["maxMessages"]=size/2;
            }
        }
        auto earliest=input.value("beforeSeq",cursor+1);
        auto newest_text=start;
        auto newest_start=start;
        std::string output;
        for (const auto& record:page.at("records")) {
            if (!record.is_object() || !record.contains("event")) continue;
            const auto& event=record.at("event");
            const auto seq=sequence(event,"seq");
            if (seq<0 || seq>cursor || seq>=input.value("beforeSeq",cursor+1)) continue;
            earliest=std::min(earliest,seq);
            const auto type=text(event,"type");
            if (type=="turn/start") newest_start=std::max(newest_start,seq);
            if (seq<=newest_text || (type!="assistant/message" && type!="assistant/response")) continue;
            const auto& data=event.at("data");
            const auto& message=data.contains("message")?data.at("message"):data;
            const auto content=message.value("content",Json());
            std::string candidate;
            if (content.is_string()) candidate=content.get<std::string>();
            else if (content.is_array()) for (const auto& part:content) {
                if (!part.is_object()) continue;
                const auto chunk=text(part,"text");
                if (!chunk.empty()) candidate+=(candidate.empty()?"":"\n")+chunk;
            }
            if (!candidate.empty()) {
                output=std::move(candidate);
                newest_text=seq;
            }
        }
        if (newest_text>newest_start) return output;
        if (newest_start>start || earliest<=start || !page.value("hasMore",false) ||
            earliest>=input.value("beforeSeq",cursor+1)) break;
        input["beforeSeq"]=earliest;
    }
    return {};
}

void observe_entries(const std::map<std::string,Entry>& entries,LegacyObservations& samples,std::stop_token stop,
                     LegacyRefresh refresh) {
    std::erase_if(samples,[&](const auto& item){return !entries.contains(item.first);});
    struct Job {
        const Entry* entry;
        LegacyObservation* sample;
        std::shared_ptr<Dsh> dsh;
        fs::path archive;
        bool acknowledged;
        bool reconcile_receipt;
    };
    Auths auths;
    std::vector<Job> jobs;
    for (const auto& [id,entry]:entries) {
        auto& sample=samples[id];
        const auto revision=Json::array({entry.record.value("startSeq",Json()),entry.record.value("cursor",Json()),
            text(entry.record,"status"),text(entry.record,"serviceOrigin"),text(entry.record,"serviceId"),
            entry.record.value("updatedAt",Json())});
        if (sample.revision!=revision) sample={.revision=revision};
        const bool handoff=refresh!=LegacyRefresh::incremental;
        const auto recent_terminal=!sample.stale && now()-sample.checked<30000 &&
            terminal(text(sample.live,"status")) && !text(sample.live,"resultVersion").empty();
        if (handoff && recent_terminal) continue;
        auto archive=archive_file(entry);
        auto dsh=refresh==LegacyRefresh::archive?std::shared_ptr<Dsh>():route(entry,auths);
        const bool has_receipt=entry.mark.is_object() && entry.mark.value("processed",false);
        const auto registered=has_receipt?projection(entry):Json::object();
        const bool acknowledged=registered.value("processed",false);
        const bool reconcile_receipt=has_receipt && !acknowledged && !archive.empty() && registered.at("status")=="failed";
        if (!handoff && (!dsh || reconcile_receipt) && !archive.empty() && recent_terminal &&
            text(sample.live,"resultSource")=="dsh_history") {
            std::error_code error;
            const auto written=fs::last_write_time(archive,error);
            if (!error && written==sample.history_write) continue;
        }
        if (!archive.empty() || dsh) {
            jobs.push_back({&entry,&sample,std::move(dsh),std::move(archive),acknowledged,reconcile_receipt});
            if (handoff) sample.stale=true;
        }
        else sample.stale=!text(entry.record,"serviceId").empty() ||
            (refresh==LegacyRefresh::incremental && !text(entry.record,"serviceOrigin").empty());
    }
    const auto is_running=[](const Job& job) {
        return !terminal(text(job.sample->live,"status",text(job.entry->record,"status")));
    };
    const auto by_age=[](const Job& a,const Job& b) {
        return std::pair(a.sample->checked,-milliseconds(a.entry->record,"updatedAt"))<
            std::pair(b.sample->checked,-milliseconds(b.entry->record,"updatedAt"));
    };
    std::vector<Job> selected;
    if (refresh!=LegacyRefresh::incremental) {
        const auto priority=[](const Job& job) {
            const auto& record=job.entry->record;
            const auto submitted=milliseconds(record,"submittedAt");
            const auto ended=milliseconds(record,"endedAt");
            return std::tuple(job.acknowledged,job.archive.empty(),ended?ended:submitted?submitted:milliseconds(record,"updatedAt"));
        };
        std::stable_sort(jobs.begin(),jobs.end(),[&](const Job& a,const Job& b){return priority(a)<priority(b);});
        selected=std::move(jobs);
    }
    else {
        const auto terminal_jobs=std::stable_partition(jobs.begin(),jobs.end(),is_running);
        std::sort(jobs.begin(),terminal_jobs,by_age);
        const auto recent_end=terminal_jobs+std::min<std::ptrdiff_t>(12,jobs.end()-terminal_jobs);
        std::partial_sort(terminal_jobs,recent_end,jobs.end(),[](const Job& a,const Job& b) {
            const auto priority=[](const Job& job) {
                return std::tuple(!job.reconcile_receipt,job.reconcile_receipt?job.sample->checked:0,
                    -milliseconds(job.entry->record,"updatedAt"));
            };
            return priority(a)<priority(b);
        });
        std::sort(recent_end,jobs.end(),by_age);
        selected.insert(selected.end(),jobs.begin(),jobs.begin()+std::min<std::ptrdiff_t>(48,terminal_jobs-jobs.begin()));
        selected.insert(selected.end(),terminal_jobs,recent_end);
        selected.insert(selected.end(),recent_end,recent_end+std::min<std::ptrdiff_t>(72-selected.size(),jobs.end()-recent_end));
        std::stable_partition(selected.begin(),selected.end(),[](const Job& job){return job.reconcile_receipt;});
    }
    std::atomic_size_t next{};
    const auto deadline=GetTickCount64()+(refresh!=LegacyRefresh::incremental?5500:3400);
    std::vector<std::jthread> workers;
    for (std::size_t i=0;i<std::min<std::size_t>(12,selected.size());++i) {
        workers.emplace_back([&] {
            while (!stop.stop_requested() && GetTickCount64()<deadline) {
                const auto index=next.fetch_add(1);
                if (index>=selected.size()) return;
                auto& job=selected[index];
                try {
                    const bool live_first=refresh==LegacyRefresh::incremental && job.dsh && !job.reconcile_receipt;
                    if (live_first) {
                        try {
                            *job.sample=observe(*job.entry,*job.dsh,*job.sample);
                            continue;
                        } catch (const std::exception&) {
                            if (job.archive.empty()) throw;
                        }
                    }
                    std::error_code stat_error;
                    const auto written=job.archive.empty()?fs::file_time_type{}:fs::last_write_time(job.archive,stat_error);
                    if (const auto history=archived_turn(*job.entry,job.archive)) {
                        job.sample->live=archived_result(*job.entry,*history);
                        job.sample->checked=now();
                        job.sample->stale=false;
                        job.sample->history_write=stat_error?fs::file_time_type{}:written;
                    } else if (!live_first && job.dsh)
                        *job.sample=observe(*job.entry,*job.dsh,*job.sample);
                    else {
                        job.sample->checked=now();
                        job.sample->stale=true;
                    }
                }
                catch (const std::exception&) { job.sample->checked=now();job.sample->stale=true; }
            }
        });
    }
    for (auto& worker:workers) worker.join();
}
}

LegacySessions read_legacy_sessions(const Json& settings,bool include_usage_candidates,LegacyObservations* observations,
                                    std::stop_token stop,LegacyRefresh refresh,const LegacyFilter& include) {
    LegacySessions result;
    auto entries=load(settings,result.sources,result.unreadable);
    if (include) std::erase_if(entries,[&](const auto& entry){return !include(entry.first);});
    if (observations && !result.sources && result.unreadable)
        throw std::runtime_error("Codex 会话登记暂时无法读取，已保留最近的会话列表");
    if (observations) observe_entries(entries,*observations,stop,refresh);
    for (const auto& [key,entry] : entries) {
        auto projected=projection(entry);
        if (observations) {
            const auto& sample=observations->at(key);
            apply_live(projected,entry,sample);
            if (sample.stale) ++result.live_stale;
            else if (sample.checked) ++result.live_checked;
        }
        result.rows[key]=std::move(projected);
        if (!include_usage_candidates) continue;
        const auto& row=entry.record;
        const auto history=row.find("history");
        const auto& first=history!=row.end() && history->is_array() && !history->empty() &&
            history->front().is_object()?history->front():row;
        result.usage_candidates[key]={{"origin",text(row,"serviceOrigin")},{"dshId",text(row,"sessionId")},
            {"first",milliseconds(first,"submittedAt")}};
    }
    return result;
}

Json legacy_result(const Json& settings,const std::string& id,const Json* observed) {
    int sources=0, unreadable=0;
    const auto entries=load(settings,sources,unreadable);
    const auto found=entries.find(id);
    if (found==entries.end()) throw std::runtime_error("原插件登记中没有该会话");
    const auto& entry=found->second;
    const auto history=archived_turn(entry,archive_file(entry));
    Auths auths;
    const auto dsh=history?std::shared_ptr<Dsh>():route(entry,auths);
    auto row=observed?*observed:live_projection(entry,dsh.get());
    const auto disk=history?archived_result(entry,*history):Json();
    const bool use_history=history && (!observed || observed->at("resultVersion")==disk.at("resultVersion"));
    if (use_history) row.update(disk);
    else if (observed && observed->value("resultSource","")=="dsh_history")
        throw DshError("历史日志的结果版本已变化","history_changed");
    Json result{{"sessionId",id},{"source","codex"},{"readOnly",true},{"resultVersion",row.at("resultVersion")}};
    if (!row.at("failure").empty()) result["failure"]=row.at("failure");
    std::string output;
    if (use_history) {
        result["resultSource"]="dsh_history";
        output=history->output;
    } else if (dsh && row.contains("liveCursor")) {
        try { output=live_output(entry,*dsh,row); }
        catch (const std::exception& error) {
            if (!result.contains("failure")) throw;
            result["readError"]=error.what();
        }
        if (output.empty() && row.at("liveCursor")==entry.record.value("cursor",Json()))
            output=text(entry.record,"assistantText");
    } else output=text(entry.record,"assistantText");
    const bool clipped=false;
    Json messages=Json::array();
    if (!output.empty()) messages.push_back({{"type","assistant/message"},{"text",output},{"truncated",clipped}});
    result["messages"]=std::move(messages);
    return result;
}

Json live_legacy_session(const Json& settings,const std::string& id) {
    int sources=0, unreadable=0;
    const auto entries=load(settings,sources,unreadable);
    const auto& entry=entries.at(id);
    if (const auto history=archived_turn(entry,archive_file(entry))) {
        auto row=projection(entry);
        apply_live(row,entry,{.live=archived_result(entry,*history)});
        return row;
    }
    Auths auths;
    return live_projection(entry,route(entry,auths).get());
}

bool complete_legacy_handoff(const Json& settings,const std::string& id,const std::string& version,
                             const std::string& outcome) {
    int sources=0, unreadable=0;
    const auto entries=load(settings,sources,unreadable);
    const auto found=entries.find(id);
    if (found==entries.end()) return false;
    auto entry=found->second;
    auto lock_path=entry.review_path;
    lock_path.replace_extension(L".lock");
    const ExclusiveStateLock lock(lock_path);
    const auto registry=read_json(entry.registry,max_registry);
    const auto& sessions=registry.at("sessions");
    const auto record=std::ranges::find_if(sessions,[&](const Json& row) {
        return row.is_object() && text(row,"sessionId")==text(entry.record,"sessionId") &&
            text(row,"serviceOrigin")==text(entry.record,"serviceOrigin") &&
            (!text(row,"serviceOrigin").empty() || row.value("serviceId",Json())==entry.record.value("serviceId",Json()));
    });
    if (record==sessions.end()) return false;
    entry.record=*record;
    Auths auths;
    const auto history=archived_turn(entry,archive_file(entry));
    const auto current=history?archived_result(entry,*history):live_projection(entry,route(entry,auths).get());
    if (current.value("stale",false)) throw std::runtime_error("暂时无法确认已领取的结果，请重试");
    if (current.at("resultVersion")!=version || version.empty() || !terminal(text(current,"status"))) return false;
    auto marks=fs::exists(entry.review_path)?read_json(entry.review_path,max_registry):Json::object();
    if (!marks.is_object()) throw std::runtime_error("Codex 回执格式无效");
    auto& mark=marks[entry.review_key];
    if (mark.is_object() && mark.value("version","")==version && mark.value("processed",false)) return true;
    mark={{"version",version},{"read",true},{"processed",true},{"outcome",outcome}};
    write_state_file(entry.review_path,marks);
    return true;
}
}
