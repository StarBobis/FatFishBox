#include "legacy.h"
#include <iostream>

int main() {
    fatfish::Cluster cluster;
    fatfish::DesktopMonitor monitor;
    for (std::string line; std::getline(std::cin,line);) {
        try {
            if (line=="migrate") cluster.migrate_repair_queue();
            std::cout << cluster.desktop_snapshot(&monitor).dump() << std::endl;
        }
        catch (const std::exception& error) {
            std::cout << fatfish::Json{{"error",error.what()}}.dump() << std::endl;
        }
    }
}
