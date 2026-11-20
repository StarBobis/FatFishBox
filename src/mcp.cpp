#include "cluster.h"
#include "mcp.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <memory>
#include <set>

namespace {
using fatfish::Json;
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
constexpr std::size_t max_request_size = 1024 * 1024;

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
        if (line.size() <= max_request_size) line.push_back(static_cast<char>(ch));
    }
    return !line.empty();
}

Json failure(const Json& id, int code, std::string message) {
    return {{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",message}}}};
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
        for (std::string line; read_request(input.get(), line);) {
            Json request_id = nullptr;
            Json output;
            try {
                if (line.size() > max_request_size) throw std::runtime_error("请求过大");
                Json message = Json::parse(line);
                if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("method")) {
                    if (!send(failure(nullptr, -32600, "Invalid request"))) return 1;
                    continue;
                }
                if (!message.contains("id")) continue;
                request_id = message.at("id");
                const auto method = message.at("method").get<std::string>();
                const auto params = message.value("params", Json::object());
                Json value;
                if (method == "initialize") {
                    const std::set<std::string> supported{"2024-11-05","2025-03-26","2025-06-18","2025-11-25"};
                    const auto version = params.value("protocolVersion", "");
                    value = {{"protocolVersion",supported.contains(version) ? version : "2025-11-25"},
                        {"capabilities",{{"tools",Json::object()},{"resources",Json::object()}}},
                        {"serverInfo",{{"name","fatfishbox"},{"title","肥鱼重工"},{"version","0.1.0"}}},
                        {"instructions","先读取 cluster_overview 或分页读取 cluster_list_sessions。派发前核对授权工作区和模型池。只确认已处理的精确结果版本。"}};
                    initialized = true;
                } else if (method == "ping") {
                    value = Json::object();
                } else if (!initialized) {
                    if (!send(failure(request_id, -32002, "Initialize before calling tools"))) return 1;
                    continue;
                } else if (method == "tools/list") {
                    value = {{"tools",Cluster::tools()}};
                } else if (method == "resources/list") {
                    value = {{"resources",Json::array({{{"uri","fatfish://workflow"},{"name","workflow"},{"mimeType","text/markdown"}}})}};
                } else if (method == "resources/templates/list") {
                    value = {{"resourceTemplates",Json::array()}};
                } else if (method == "resources/read" && params.value("uri","") == "fatfish://workflow") {
                    value = {{"contents",Json::array({{{"uri","fatfish://workflow"},{"mimeType","text/markdown"},
                        {"text","# 肥鱼重工\n读取状态后按账号和模型池并发派发。结果按版本读取和确认。项目验收由主会话负责。"}}})}};
                } else if (method == "tools/call") {
                    try {
                        auto data = cluster.invoke(params.at("name").get<std::string>(), params.value("arguments", Json::object()));
                        value = {{"content",Json::array()},{"structuredContent",std::move(data)}};
                    } catch (const std::exception& error) {
                        value = {{"isError",true},{"content",Json::array({{{"type","text"},{"text",error.what()}}})}};
                    }
                } else {
                    if (!send(failure(request_id, -32601, "Method not found"))) return 1;
                    continue;
                }
                output = {{"jsonrpc","2.0"},{"id",request_id},{"result",std::move(value)}};
            } catch (const Json::parse_error&) {
                output = failure(nullptr, -32700, "Invalid JSON");
            } catch (const std::exception&) {
                output = failure(request_id, -32602, "Invalid request arguments");
            }
            if (!send(output)) return 1;
        }
    } catch (const std::exception& error) {
        if (const auto diagnostics = open_stream(STD_ERROR_HANDLE, "wb", _O_WRONLY))
            std::fprintf(diagnostics.get(), "%s\n", error.what());
        return 1;
    }
    return std::ferror(input.get()) ? 1 : 0;
}
