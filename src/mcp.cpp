#include "mcp.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <memory>
#include <array>

namespace {
using fatfish::Json;
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
constexpr auto workflow=R"workflow(# 肥鱼重工 · 主代理工作流

以下示例以「第 15 轮透明度蒙版」为例。将路径、池 ID 和提交哈希替换成实际值。

1. 主代理确定本轮基线和验收条件。在主仓库核对 HEAD 后，从该提交创建隔离工作树，例如 `git worktree add -b codex/mask-b15 C:/Dev/worktrees/mask-b15 HEAD`。为每个并行子会话准备独立工作树。
2. 用 `cluster_get_settings` 查询已启用模型的池 ID，然后调用 `cluster_start_session`。例如 `{"batch":"mask-b15","title":"实现透明度蒙版","workspace":"C:/Dev/worktrees/mask-b15","allowedPoolIds":["<GitHub 池 ID>","<Outlook 池 ID>"],"task":"实现蒙版并运行专项测试。提交候选，保存截图，报告实际命令与退出码。"}`。批次首次设置允许池后，后续派发及返修沿用该清单。省略 `poolId` 可在范围内自动换池。若返回派发诊断，检查 `pools[].reason`、`occupied`、`concurrency` 和 `availablePools`。提交状态不确定时先同步，确认原任务状态后再决定是否重试。
3. 子会话负责实现、自测和证据，最终回复包含 `fatfish-delivery` 围栏里的 JSON，例如：

```fatfish-delivery
{"candidateSha":"<完整 Git SHA>","worktreeClean":true,"tests":[{"command":"ctest -C Release -R mask","exitCode":0}],"evidencePaths":["C:/Dev/worktrees/mask-b15/build/mask.png"]}
```

4. 主代理调用 `cluster_next_handoff({})` 领队首，保存 `claimToken` 和 `session.resultVersion`。核对 `delivery` 中自报的提交、工作树、测试和证据。主代理在候选上跑累计构建和回归，亲自打开实窗检查布局、交互以及截图覆盖不到的行为。
5. 发现问题时，从已核对的候选 SHA 建返修工作树，例如 `git worktree add -b codex/mask-b15-repair C:/Dev/worktrees/mask-b15-repair <候选 SHA>`。调用 `cluster_repair_handoff`，传入 `claimToken`、原 `session.resultVersion` 作为 `originalVersion`、稳定的 `requestId`、`candidateSha`、`screenshotPaths`、`acceptance`、`title`、返修 `workspace` 和 `task`。例如验收条件可以写「在目标缩放下打开实窗，标题与按钮仍可读，累计构建和蒙版回归通过」。网络重试时沿用同一 `requestId`。
6. 同批次已有返修时，调用 `cluster_attach_handoff_repair({claimToken,originalVersion,sessionId})` 绑定已派发会话。使用 `cluster_get_session_status({sessionId})` 看队列状态或是否需要输入，查询时可填派发返回的 `id` 或 `dshId`。若 `stale` 为 true，稍后重查。
7. 用 `cluster_next_handoff({claimToken})` 重读队首。返修结束后，从 `repair.delivery` 获取候选并重新做累计验证和实窗验收。确认实际处置后提交 `cluster_next_handoff({claimToken,outcome:"已核对返修提交、完成累计构建和实窗验收，已合入。"})`。工具确认当前交接并返回下一条。已领取凭据过期时，用空参数重新领取原队首。
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
}

bool fatfish::mcp_protocol_supported(std::string_view version) {
    constexpr std::array<std::string_view,4> versions{"2024-11-05","2025-03-26","2025-06-18","2025-11-25"};
    return std::find(versions.begin(),versions.end(),version)!=versions.end();
}

fatfish::Json fatfish::dispatch_mcp(const Cluster& cluster,std::string_view line,bool* initialized) {
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
        Json value;
        if (method=="initialize") {
            const auto version=params.value("protocolVersion","");
            value={{"protocolVersion",mcp_protocol_supported(version)?version:"2025-11-25"},
                {"capabilities",{{"tools",Json::object()},{"resources",Json::object()}}},
                {"serverInfo",{{"name","fatfishbox"},{"title","肥鱼重工"},{"version","0.1.0"}}},
                {"instructions","完成对话由 cluster_next_handoff 按完成时间逐条领取。实际处置后提交当前领取凭据与处置结果。"}};
            if (initialized) *initialized=true;
        } else if (method=="ping") {
            value=Json::object();
        } else if (initialized && !*initialized) {
            return failure(request_id,-32002,"Initialize before calling tools");
        } else if (method=="tools/list") {
            value={{"tools",Cluster::tools()}};
        } else if (method=="resources/list") {
            value={{"resources",Json::array({{{"uri","fatfish://workflow"},{"name","workflow"},{"mimeType","text/markdown"}}})}};
        } else if (method=="resources/templates/list") {
            value={{"resourceTemplates",Json::array()}};
        } else if (method=="resources/read" && params.value("uri","")=="fatfish://workflow") {
            value={{"contents",Json::array({{{"uri","fatfish://workflow"},{"mimeType","text/markdown"},
                {"text",workflow}}})}};
        } else if (method=="tools/call") {
            try {
                const auto tool=params.at("name").get<std::string>();
                const auto available=Cluster::tools();
                if (std::ranges::none_of(available,[&](const Json& entry){return entry.at("name")==tool;}))
                    throw std::runtime_error("此操作不对 MCP 开放");
                auto data=cluster.invoke(tool,params.value("arguments",Json::object()));
                if (tool=="cluster_refresh_sessions") data={{"synced",data.at("sessions").size()},
                    {"errors",data.at("errors").size()},{"total",data.at("total")}};
                if (tool=="cluster_cancel_session") data={{"status",data.at("status")}};
                value={{"content",Json::array()},{"structuredContent",std::move(data)}};
            } catch (const fatfish::DispatchError& error) {
                value={{"isError",true},{"structuredContent",error.details},
                    {"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
            } catch (const std::exception& error) {
                value={{"isError",true},{"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
            }
        } else {
            return failure(request_id,-32601,"Method not found");
        }
        return {{"jsonrpc","2.0"},{"id",request_id},{"result",std::move(value)}};
    } catch (const Json::parse_error&) {
        return failure(nullptr,-32700,"Invalid JSON");
    } catch (const std::exception&) {
        return failure(request_id,-32602,"Invalid request arguments");
    }
}

void fatfish::mcp_diagnostic(std::string_view message) {
    if (const auto diagnostics=open_stream(STD_ERROR_HANDLE,"wb",_O_WRONLY)) {
        std::fwrite(message.data(),1,message.size(),diagnostics.get());
        std::fputc('\n',diagnostics.get());
    }
}

int fatfish::run_mcp_stdio() {
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
        bool initialized = false;
        for (std::string line; read_request(input.get(),line);) {
            const auto response=dispatch_mcp(cluster,line,&initialized);
            if (!response.is_null() && !send(response)) return 1;
        }
    } catch (const std::exception& error) {
        mcp_diagnostic(error.what());
        return 1;
    }
    return std::ferror(input.get()) ? 1 : 0;
}
