#include <httplib.h>
#include <windows.h>
#include <bcrypt.h>
#include "mcp.h"
#include <array>
#include <charconv>
#include <fstream>
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

std::string read_token(const fs::path& file) {
    std::array<char,65> bytes{};
    std::ifstream stream(file,std::ios::binary);
    stream.read(bytes.data(),bytes.size());
    if (stream.bad() || stream.gcount()!=64 ||
        !std::all_of(bytes.begin(),bytes.begin()+64,[](char ch){return (ch>='0' && ch<='9') || (ch>='a' && ch<='f');}))
        throw std::runtime_error("MCP 令牌文件无效或不可读取");
    return {bytes.data(),64};
}

std::string load_token(const fs::path& folder) {
    const auto file=folder/L"mcp-http.token";
    if (fs::exists(file)) return read_token(file);
    std::array<unsigned char,32> entropy{};
    if (BCryptGenRandom(nullptr,entropy.data(),static_cast<ULONG>(entropy.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
        throw std::runtime_error("MCP 令牌生成失败");
    constexpr char digits[]="0123456789abcdef";
    std::string value;
    for (const auto byte:entropy) { value+=digits[byte>>4]; value+=digits[byte&15]; }
    const auto temp=folder/path_from_utf8("mcp-token-"+id()+".tmp");
    try {
        std::ofstream stream(temp,std::ios::binary);
        stream.write(value.data(),static_cast<std::streamsize>(value.size()));
        stream.close();
        if (!stream) throw std::runtime_error("MCP 令牌写入失败");
        if (MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH)) return value;
        if (!fs::exists(file)) throw std::runtime_error("MCP 令牌保存失败");
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp,ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(temp,ignored);
    return read_token(file);
}

bool token_matches(std::string_view supplied,std::string_view expected) {
    if (supplied.size()!=expected.size()) return false;
    unsigned difference{};
    for (std::size_t i=0;i<expected.size();++i) difference|=supplied[i]^expected[i];
    return !difference;
}
}

struct McpHttpServer::Impl {
    const int port;
    const std::string host, local_host, key;
    httplib::Server server;
    std::thread worker;

    explicit Impl(const Cluster& cluster)
        : port(port_number()),host("127.0.0.1:"+std::to_string(port)),
          local_host("localhost:"+std::to_string(port)),key(load_token(cluster.data_directory())) {
        server.set_address_family(AF_INET);
        server.set_socket_options([](socket_t socket) {
            httplib::set_socket_opt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,1);
        });
        server.set_payload_max_length(mcp_max_request_size);
        server.set_read_timeout(3,0);
        server.set_write_timeout(5,0);
        server.set_keep_alive_max_count(1);
        server.new_task_queue=[] { return new httplib::ThreadPool(4,4,16); };
        server.set_pre_routing_handler([this,bearer="Bearer "+key](const httplib::Request& request,httplib::Response& response) {
            using enum httplib::Server::HandlerResponse;
            if (request.path!="/mcp") return Unhandled;
            response.set_header("Cache-Control","no-store");
            const auto target=request.get_header_value("Host");
            const auto origin=request.get_header_value("Origin");
            if (request.get_header_value_count("Host")!=1 || (target!=host && target!=local_host) ||
                request.get_header_value_count("Origin")>1 ||
                (request.has_header("Origin") && origin!="http://"+host && origin!="http://"+local_host)) {
                response.status=403;
                return Handled;
            }
            if (request.get_header_value_count("Authorization")!=1 ||
                !token_matches(request.get_header_value("Authorization"),bearer)) {
                response.status=401;
                response.set_header("WWW-Authenticate","Bearer realm=\"FatFishBox\"");
                return Handled;
            }
            if (request.method!="POST") {
                response.status=405;
                response.set_header("Allow","POST");
                return Handled;
            }
            return Unhandled;
        });
        server.Post("/mcp",[&cluster](const httplib::Request& request,httplib::Response& response) {
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
            const auto message=dispatch_mcp(cluster,request.body);
            if (message.is_null()) { response.status=202; return; }
            response.set_content(message.dump(-1,' ',false,Json::error_handler_t::replace),"application/json");
        });
        if (!server.bind_to_port("127.0.0.1",port))
            throw std::runtime_error("本机 MCP 端口 "+std::to_string(port)+" 无法监听，可能已被占用");
        worker=std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~Impl() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
};

McpHttpServer::McpHttpServer(const Cluster& cluster):impl_(std::make_unique<Impl>(cluster)) {}
McpHttpServer::~McpHttpServer()=default;
void McpHttpServer::wait() { if (impl_->worker.joinable()) impl_->worker.join(); }
std::string McpHttpServer::url() const { return "http://"+impl_->host+"/mcp"; }
const std::string& McpHttpServer::token() const { return impl_->key; }

int run_mcp_http() {
    try {
        const Cluster cluster;
        McpHttpServer server(cluster);
        mcp_diagnostic(server.url());
        server.wait();
        return 0;
    } catch (const std::exception& error) {
        mcp_diagnostic(error.what());
        return 1;
    }
}
}
