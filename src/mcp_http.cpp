#include "diagnostics.h"
#include <httplib.h>
#include <windows.h>
#include "mcp.h"
#include <charconv>
#include <stdexcept>
#include <thread>

namespace fatfish {
namespace {
int port_number() {
    char input[8]{};
    const auto length=GetEnvironmentVariableA("FATFISHBOX_MCP_PORT",input,sizeof(input));
    if (!length) return 17863;
    int port{};
    if (length>=sizeof(input)) throw std::runtime_error("FATFISHBOX_MCP_PORT 无效");
    const auto [end,error]=std::from_chars(input,input+length,port);
    if (error!=std::errc{} || end!=input+length || port<1024 || port>65535)
        throw std::runtime_error("FATFISHBOX_MCP_PORT 需要在 1024 至 65535 之间");
    return port;
}

}

struct McpHttpServer::Impl {
    const int port;
    const std::string host, local_host;
    httplib::Server server;
    std::thread worker;
    std::jthread prechecks;

    explicit Impl(const Cluster& cluster)
        : port(port_number()),host("127.0.0.1:"+std::to_string(port)),
          local_host("localhost:"+std::to_string(port)) {
        server.set_exception_handler([](const httplib::Request&,httplib::Response& response,std::exception_ptr exception) {
            try { if (exception) std::rethrow_exception(exception); }
            catch (...) { diagnostics::caught(); }
            response.status=500;
            response.set_content("Internal error; see executable logs directory","text/plain");
        });
        server.set_address_family(AF_INET);
        server.set_socket_options([](socket_t socket) {
            httplib::set_socket_opt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,1);
        });
        server.set_payload_max_length(mcp_max_request_size);
        server.set_read_timeout(3,0);
        server.set_write_timeout(5,0);
        server.set_keep_alive_max_count(1);
        server.new_task_queue=[] { return new httplib::ThreadPool(4,4,16); };
        server.set_pre_routing_handler([this](const httplib::Request& request,httplib::Response& response) {
            using enum httplib::Server::HandlerResponse;
            if (request.path!="/mcp" && request.path!="/team") return Unhandled;
            response.set_header("Cache-Control","no-store");
            const auto target=request.get_header_value("Host");
            const auto origin=request.get_header_value("Origin");
            if (request.get_header_value_count("Host")!=1 || (target!=host && target!=local_host) ||
                request.get_header_value_count("Origin")>1 ||
                (request.has_header("Origin") && origin!="http://"+host && origin!="http://"+local_host)) {
                response.status=403;
                return Handled;
            }
            if (request.method!="POST") {
                response.status=405;
                response.set_header("Allow","POST");
                return Handled;
            }
            return Unhandled;
        });
        const auto handle=[&cluster](const httplib::Request& request,httplib::Response& response) {
            if (request.has_header("MCP-Protocol-Version") &&
                !mcp_protocol_supported(request.get_header_value("MCP-Protocol-Version"))) {
                response.status=400;
                return;
            }
            const auto content_type=request.get_header_value("Content-Type");
            if (content_type.substr(0,content_type.find(';'))!="application/json") {
                response.status=415;
                return;
            }
            const auto message=dispatch_mcp(cluster,request.body,nullptr,request.path=="/team");
            if (message.is_null()) { response.status=202; return; }
            response.set_content(message.dump(-1,' ',false,Json::error_handler_t::replace),"application/json");
        };
        server.Post("/mcp",handle);
        server.Post("/team",handle);
        if (!server.bind_to_port("127.0.0.1",port))
            throw std::runtime_error("本机 MCP 端口 "+std::to_string(port)+" 无法监听，可能已被占用");
        worker=std::thread([this] { try { server.listen_after_bind(); } catch (...) { diagnostics::caught(); } });
        server.wait_until_ready();
        prechecks=std::jthread([&cluster](std::stop_token stop) {cluster.run_prechecks(stop);});
    }
    ~Impl() {
        prechecks.request_stop();
        server.stop();
        if (worker.joinable()) worker.join();
    }
};

McpHttpServer::McpHttpServer(const Cluster& cluster):impl_(std::make_unique<Impl>(cluster)) {}
McpHttpServer::~McpHttpServer()=default;
void McpHttpServer::wait() { if (impl_->worker.joinable()) impl_->worker.join(); }
std::string McpHttpServer::url() const { return "http://"+impl_->host+"/mcp"; }

int run_mcp_http() {
    try {
        const Cluster cluster;
        McpHttpServer server(cluster);
        mcp_diagnostic(server.url());
        server.wait();
        return 0;
    } catch (const std::exception& error) { fatfish::diagnostics::caught();
        mcp_diagnostic(error.what());
        return 1;
    }
}
}
