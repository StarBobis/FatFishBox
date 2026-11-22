#pragma once
#include "cluster.h"
namespace fatfish {
bool synchronize_work_items(Json& state);
std::string session_work_item(const Json& state,const Json& row,const std::string& queue);
Json create_work_item(Json& state,const std::string& queue,const Json& input);
Json update_work_item(Json& state,const std::string& queue,const Json& input,const Json& observed=Json());
Json query_work_items(const Json& state,const std::string& queue,const Json& input,bool single);
void register_work_session(Json& state,Json& row,const Json& input,const std::string& queue);
void reassign_work_session_queue(Json& state,const std::string& session,const std::string& queue);
std::string publish_work_item_spec(const fs::path& data,const Json& item);
void observe_work_sessions(Json& state,const std::string& queue,const Json& rows);
}
