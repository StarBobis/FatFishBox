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
constexpr auto workflow=R"workflow(# FatFishBox simple team workflow
Read team_get_prompt(principal). The principal plans only the requested changes and starts one persistent-goal manager. Use team_wait_decisions for up to 300000 ms; keep a pending call alive and renew quiet timeouts silently. Act on decisions and explicit errors.
The manager calls team_dispatch_ready and handles team_next_action FIFO. Developers implement and perform relevant self-tests. Arrange independent review and repair real defects. The manager confirms completion using judgement; team_verify_candidate is optional, never a mandatory acceptance gate.
Do not collect proof, hashes, manifests, screenshot archives or exhaustive page/configuration/frame matrices. Do not expand a local fix into a full-product audit. Stop extra proof-only work from older checklists while preserving development/testing and existing sessions. Report actual results and skipped coverage honestly. No verificationId, evidence array or acceptanceResults proof list is required.
Use MCP team_wait_actions while workers/decisions remain. After all current tasks are confirmed, team_finish_domain with requestId/summary, then complete the goal. The principal reviews actual changes and integrates main through team_record_merge or gives a clear repair plan. Retire obsolete terminal work through team_retire_work_item, never fake repairs. Keep ordinary ownership, valid Git references and stable IDs for correct bookkeeping.
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
            result.erase(std::remove_if(result.begin(),result.end(),[](const Json& t){const auto name=t.at("name").get<std::string>();return name=="team_capture_evidence" || name=="cluster_capture_evidence" || name=="cluster_get_evidence_manifest";}),result.end());
            if (principal_only) {
                const std::set<std::string> allowed{"team_get_settings","team_get_prompt","team_start_project","team_activate_project","team_get_project","team_list_projects","team_get_queue","team_wait_decisions","team_resolve_decision","team_update_charter","team_control_project","team_record_merge","team_capture_evidence","team_retire_work_item"};
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
                {"instructions",principal_only?"Read team_get_prompt(principal), save a draft with team_start_project, then call team_activate_project once the full plan is ready. Wait only through team_wait_decisions for up to 300000 ms; continue pending calls and renew quiet timeouts silently using the cursor. Act on decisions and explicit errors. After manager completion, review the actual changes and merge each item, or provide a complete repair plan.":mcp_handoff_instructions}};
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
            std::string tool;
            try {
                tool=params.at("name").get<std::string>();
                const auto available=available_tools();
                if (tool!="team_capture_evidence" && tool!="cluster_capture_evidence" && std::ranges::none_of(available,[&](const Json& entry){return entry.at("name")==tool;}))
                    throw std::runtime_error("此操作不对 MCP 开放");
                auto data=invoke_tool(cluster,tool,params.value("arguments",Json::object()));
                if (tool=="cluster_refresh_sessions") data={{"synced",data.at("sessions").size()},
                    {"errors",data.at("errors").size()},{"total",data.at("total")},{"queueId",data.at("queueId")}};
                if (tool=="cluster_cancel_session") data={{"status",data.at("status")},{"queueId",data.at("queueId")}};
                // Some clients only project text blocks into the model's tool result.
                auto content=Json::array({model_visible_payload(data)});
                value={{"content",std::move(content)},{"structuredContent",std::move(data)}};
            } catch (const fatfish::DispatchError& error) { fatfish::diagnostics::caught();
                auto details=error.details;
                if (tool.starts_with("team_") && !details.contains("code")) {
                    details["code"]=details.value("reason","team_request_failed");
                    details["details"]=error.details;
                    details["message"]=error.what();
                    details["allowedActions"]=Json::array();
                }
                value={{"isError",true},{"structuredContent",details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(details)})}};
            } catch (const fatfish::DshError& error) { fatfish::diagnostics::caught();
                auto details=fatfish::dsh_error_diagnostics(error);
                if (tool.starts_with("team_")) {
                    details["code"]=details.at("reason");
                    details["details"]=fatfish::dsh_error_diagnostics(error);
                    details["message"]=error.what();
                    details["allowedActions"]=Json::array();
                }
                value={{"isError",true},{"structuredContent",details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}},model_visible_payload(details)})}};
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                if (tool.starts_with("team_")) {
                    auto details=Json{{"code","team_request_failed"},{"message",error.what()},{"details",Json::object()},{"allowedActions",Json::array()}};
                    value={{"isError",true},{"structuredContent",details},{"content",Json::array({model_visible_payload(details)})}};
                } else value={{"isError",true},{"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
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
