#include "diagnostics.h"
#include "mcp.h"
#include "team.h"
#include "dsh.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <memory>
#include <semaphore>
#include <thread>
#include <array>
#include <set>

namespace {
using fatfish::Json;
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
constexpr auto workflow=R"workflow(# FatFishBox two-loop workflow
Read fatfish://team/principal or team_get_prompt(principal). The principal understands the requirement, commits a complete plan with task scope, steps, review attacks, build/test commands, acceptance evidence and repair handling, and starts one manager with a real persistent goal.
The principal must call team_wait_decisions for up to 300000 ms. Handle early events immediately and read team_get_project. Do not sleep, fill unused wait time, consume developer handoffs or take over daily scheduling.
Tasks require name/steps/acceptance/files/dependsOn/startCondition/tests. The manager reads team_domain_context, queues all ready work with team_dispatch_ready, and handles team_next_action FIFO. Developers and repair workers must first build and test their candidates and report successful command/exitCode results; log attachments are advisory. Then dispatch independent review. After review passes, the manager personally runs team_verify_candidate in the exact candidate branch and accepts only with passed=true and its verificationId. Failed checks require repair and fresh review.
While workers or decisions remain, the manager stays in the same goal and waits exclusively through team_wait_actions for up to 300000 ms. Actionable results wake MCP immediately; do not sleep or fill the remaining time.
Retire obsolete terminal items through team_retire_work_item, retaining history without fake repair jobs. Ordinary missing attachments are advisory warnings. Managers may collect/correct declared or in-worktree files through team_capture_evidence; original references and real replacement hashes are retained. Report/SHA mismatches return expected/actual values and pause that result. Genuinely blocked FIFO actions can be escalated by actionId/jobId instead of spinning.
After all current planned items are accepted, team_finish_domain and complete the DSH goal. No integrator or preliminary merge is required. After the manager exits, the principal independently accepts, actually merges main and calls team_record_merge. Failed final acceptance requires a new complete charter/tasks plan through team_update_charter, then MCP waiting resumes.
Legacy cluster tools remain compatible. Manager tools require project credentials; stable requestId, exact candidate SHA and resultVersion prevent duplicate dispatch and stale acceptance. Old records are never automatically converted into passing evidence.
)workflow";

File open_stream(DWORD standard_handle, const char* mode, int flags) {
    HANDLE handle = GetStdHandle(standard_handle);
    HANDLE duplicate{};
    if (!handle || handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) return File(nullptr, &std::fclose);
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), flags | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(duplicate);
        return File(nullptr, &std::fclose);
    }
    auto* stream = _fdopen(descriptor, mode);
    if (!stream) _close(descriptor);
    return File(stream, &std::fclose);
}

bool read_request(FILE* input, std::string& line) {
    line.clear();
    for (int ch; (ch = std::fgetc(input)) != EOF;) {
        if (ch == '\n') return true;
        if (line.size() <= fatfish::mcp_max_request_size) line.push_back(static_cast<char>(ch));
    }
    return !line.empty();
}

Json failure(const Json& id, int code, std::string message) {
    return {{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",message}}}};
}

Json model_visible_payload(const Json& data) {
    return {{"type","text"},{"text",data.dump(-1,' ',false,Json::error_handler_t::replace)}};
}
Json invoke_tool(const fatfish::Cluster& cluster,std::string_view tool,const Json& arguments) {
    if (tool!="cluster_wait_handoff" && tool!="cluster_wait_events" && tool!="team_wait_decisions" && tool!="team_wait_actions") return cluster.invoke_mcp(tool,arguments);
    // Leave HTTP workers available for ordinary requests.
    static std::counting_semaphore<2> slots{2};
    if (!slots.try_acquire()) throw fatfish::DispatchError("等待调用已满，请稍后重试",
        {{"reason","wait_capacity"},{"retryAfterMs",1000}});
    struct ReleaseSlot { ~ReleaseSlot() { slots.release(); } } release;
    return cluster.invoke_mcp(tool,arguments);
}
}

bool fatfish::mcp_protocol_supported(std::string_view version) {
    constexpr std::array<std::string_view,4> versions{"2024-11-05","2025-03-26","2025-06-18","2025-11-25"};
    return std::find(versions.begin(),versions.end(),version)!=versions.end();
}

fatfish::Json fatfish::dispatch_mcp(const Cluster& cluster,std::string_view line,bool* initialized,bool principal_only) {
    Json request_id=nullptr;
    try {
        if (line.size()>mcp_max_request_size) return failure(nullptr,-32600,"Request too large");
        const auto message=Json::parse(line);
        if (!message.is_object() || !message.contains("jsonrpc") || message.at("jsonrpc")!="2.0")
            return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("method") && !message.contains("id")) return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("method")) {
            if (message.contains("result") || message.contains("error")) return nullptr;
            return failure(nullptr,-32600,"Invalid request");
        }
        if (!message.at("method").is_string()) return failure(nullptr,-32600,"Invalid request");
        if (!message.contains("id")) return nullptr;
        request_id=message.at("id");
        if (!request_id.is_string() && !request_id.is_number_integer() && !request_id.is_null())
            return failure(nullptr,-32600,"Invalid request id");
        const auto method=message.at("method").get<std::string>();
        const auto params=message.value("params",Json::object());
        const auto available_tools=[&] {
            auto result=principal_only?team_tools():Cluster::tools();
            if (principal_only) {
                const std::set<std::string> allowed{"team_get_settings","team_get_prompt","team_start_project","team_get_project","team_list_projects","team_get_queue","team_wait_decisions","team_resolve_decision","team_update_charter","team_control_project","team_record_merge","team_capture_evidence","team_retire_work_item"};
                auto filtered=Json::array();for (auto& entry:result) if (allowed.contains(entry.at("name").get<std::string>())) filtered.push_back(std::move(entry));return filtered;
            }
            return result;
        };
        Json value;
        if (method=="initialize") {
            const auto version=params.value("protocolVersion","");
            value={{"protocolVersion",mcp_protocol_supported(version)?version:"2025-11-25"},
                {"capabilities",{{"tools",Json::object()},{"resources",Json::object()}}},
                {"serverInfo",{{"name","fatfishbox"},{"title","肥鱼重工"},{"version",FATFISHBOX_VERSION}}},
                {"instructions",principal_only?"Read team_get_prompt(principal), commit a complete task plan, and start one persistent-goal manager. Wait only through team_wait_decisions for up to 300000 ms; handle early feedback immediately. After manager completion, independently accept and merge each item, or provide a complete repair plan.":mcp_handoff_instructions}};
            if (initialized) *initialized=true;
        } else if (method=="ping") {
            value=Json::object();
        } else if (initialized && !*initialized) {
            return failure(request_id,-32002,"Initialize before calling tools");
        } else if (method=="tools/list") {
            value={{"tools",available_tools()}};
        } else if (method=="resources/list") {
            value={{"resources",Json::array({{{"uri",principal_only?"fatfish://team/principal":"fatfish://workflow"},{"name",principal_only?"principal":"workflow"},{"mimeType","text/markdown"}}})}};
        } else if (method=="resources/templates/list") {
            value={{"resourceTemplates",Json::array()}};
        } else if (method=="resources/read" && params.value("uri","")=="fatfish://team/principal") {
            value={{"contents",Json::array({{{"uri","fatfish://team/principal"},{"mimeType","text/markdown"},{"text",team_prompt("principal")}}})}};
        } else if (method=="resources/read" && !principal_only && params.value("uri","")=="fatfish://workflow") {
            value={{"contents",Json::array({{{"uri","fatfish://workflow"},{"mimeType","text/markdown"},
                {"text",workflow}}})}};
        } else if (method=="tools/call") {
            try {
                const auto tool=params.at("name").get<std::string>();
                const auto available=available_tools();
                if (std::ranges::none_of(available,[&](const Json& entry){return entry.at("name")==tool;}))
                    throw std::runtime_error("此操作不对 MCP 开放");
                auto data=invoke_tool(cluster,tool,params.value("arguments",Json::object()));
                if (tool=="cluster_refresh_sessions") data={{"synced",data.at("sessions").size()},
                    {"errors",data.at("errors").size()},{"total",data.at("total")},{"queueId",data.at("queueId")}};
                if (tool=="cluster_cancel_session") data={{"status",data.at("status")},{"queueId",data.at("queueId")}};
                // Some clients only project text blocks into the model's tool result.
                auto content=Json::array({model_visible_payload(data)});
                value={{"content",std::move(content)},{"structuredContent",std::move(data)}};
            } catch (const fatfish::DispatchError& error) { fatfish::diagnostics::caught();
                value={{"isError",true},{"structuredContent",error.details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(error.details)})}};
            } catch (const fatfish::DshError& error) { fatfish::diagnostics::caught();
                auto details=fatfish::dsh_error_diagnostics(error);
                value={{"isError",true},{"structuredContent",details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(details)})}};
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                value={{"isError",true},{"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
            }
        } else {
            return failure(request_id,-32601,"Method not found");
        }
        return {{"jsonrpc","2.0"},{"id",request_id},{"result",std::move(value)}};
    } catch (const Json::parse_error&) { fatfish::diagnostics::caught();
        return failure(nullptr,-32700,"Invalid JSON");
    } catch (const std::exception&) { fatfish::diagnostics::caught();
        return failure(request_id,-32602,"Invalid request arguments");
    } catch (...) { fatfish::diagnostics::caught();
        return failure(request_id,-32603,"Internal error; see executable logs directory");
    }
}

void fatfish::mcp_diagnostic(std::string_view message) {
    if (const auto diagnostics=open_stream(STD_ERROR_HANDLE,"wb",_O_WRONLY)) {
        std::fwrite(message.data(),1,message.size(),diagnostics.get());
        std::fputc('\n',diagnostics.get());
    }
}

int fatfish::run_mcp_stdio(bool principal_only) {
    const auto input = open_stream(STD_INPUT_HANDLE, "rb", _O_RDONLY);
    const auto response_stream = open_stream(STD_OUTPUT_HANDLE, "wb", _O_WRONLY);
    if (!input || !response_stream) return 1;
    const auto send = [&](const Json& value) {
        const auto line = value.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        return std::fwrite(line.data(), 1, line.size(), response_stream.get()) == line.size() &&
            std::fflush(response_stream.get()) == 0;
    };
    try {
        Cluster cluster;
        std::jthread prechecks([&](std::stop_token stop) {cluster.run_prechecks(stop);});
        bool initialized = false;
        for (std::string line; read_request(input.get(),line);) {
            const auto response=dispatch_mcp(cluster,line,&initialized,principal_only);
            if (!response.is_null() && !send(response)) return 1;
        }
    } catch (const std::exception& error) { fatfish::diagnostics::caught();
        mcp_diagnostic(error.what());
        return 1;
    }
    return std::ferror(input.get()) ? 1 : 0;
}
