#include "legacy.h"
#include "dsh.h"
#include <iostream>

int main() {
    fatfish::Cluster cluster;
    fatfish::DesktopMonitor monitor;
    for (std::string line; std::getline(std::cin,line);) {
        try {
            if (line=="extract-desktop-plugin") {
                std::cout << fatfish::Json{{"path",fatfish::utf8(fatfish::extract_dsh_desktop_plugin())}}.dump() << std::endl;
                continue;
            }
            if (line=="authorize-desktop") {
                fatfish::authorize_dsh_desktop();
                std::cout << "{\"authorized\":true}" << std::endl;
                continue;
            }
            if (line.starts_with("fail:")) {
                std::cout << fatfish::Json{{"session",cluster.mark_desktop_failed(line.substr(5))}}.dump() << std::endl;
                continue;
            }
            if (line.starts_with("invoke:")) {
                const auto request=fatfish::Json::parse(line.substr(7));
                std::cout << cluster.invoke_mcp(request.at("tool").get<std::string>(),request.at("args")).dump() << std::endl;
                continue;
            }
            if (line=="migrate") cluster.migrate_repair_queue();
            if (line=="usage") cluster.refresh_usage({});
            if (line.starts_with("delete:")) cluster.delete_desktop_queue(line.substr(7));
            const auto scoped=line.starts_with("show:")?cluster.for_queue(line.substr(5)):cluster;
            std::cout << scoped.desktop_snapshot(&monitor).dump() << std::endl;
        }
        catch (const std::exception& error) {
            std::cout << fatfish::Json{{"error",error.what()}}.dump() << std::endl;
        }
    }
}
