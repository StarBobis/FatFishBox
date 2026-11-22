#pragma once

#include "dsh.h"

namespace fatfish {
struct DshHistory {
    TurnEnd ending;
    std::int64_t cursor;
    std::string output;
};

fs::path dsh_history_file(const std::string& session,const std::string& workspace);
std::optional<DshHistory> read_dsh_history(const fs::path& file,const std::string& session,
    std::int64_t after,std::optional<std::int64_t> through={});
}
