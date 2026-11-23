#pragma once
#include "cluster.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>
namespace fatfish {
inline std::string plan_scope(std::string path) {
    std::replace(path.begin(),path.end(),'\\','/');
    std::transform(path.begin(),path.end(),path.begin(),[](unsigned char c){return c>='A' && c<='Z'?static_cast<char>(c+32):static_cast<char>(c);});
    return path;
}
inline bool plan_scopes_overlap(const std::string& a,const std::string& b) {
    const auto x=plan_scope(a),y=plan_scope(b);
    const auto contains=[](const std::string& scope,const std::string& file) {
        return scope==file || (scope.ends_with("/**") && (file==scope.substr(0,scope.size()-3) || file.starts_with(scope.substr(0,scope.size()-2))));
    };
    return contains(x,y) || contains(y,x);
}
inline std::string task_plan_error(const Json& tasks,std::size_t maximum) {
    if (!tasks.is_array() || tasks.empty() || tasks.size()>maximum) return "tasks必须是非空数组，数量不超过扣除项目经理后的可用总并发";
    std::map<std::string,std::size_t> names;
    for (std::size_t i=0;i<tasks.size();++i) {
        const auto& t=tasks.at(i);
        if (!t.is_object()) return "计划项必须是对象";
        for (const auto* k:{"name","steps","acceptance","startCondition","tests"})
            if (!t.contains(k) || !t.at(k).is_string() || t.at(k).get_ref<const std::string&>().find_first_not_of(" \r\n\t")==std::string::npos) return std::string("计划缺少非空文本字段：")+k;
        if (t.at("name").get_ref<const std::string&>().size()>200 || t.at("steps").get_ref<const std::string&>().size()>12000 ||
            t.at("acceptance").get_ref<const std::string&>().size()>6000 || t.at("tests").get_ref<const std::string&>().size()>6000 || t.at("startCondition").get_ref<const std::string&>().size()>4000) return "计划字段超出长度上限";
        if (!names.emplace(t.at("name").get<std::string>(),i).second) return "计划任务名称须唯一";
        if (!t.contains("dependsOn") || !t.at("dependsOn").is_array()) return "每项任务必须显式提供dependsOn数组；无依赖用[]";
        if (!t.contains("files") || !t.at("files").is_array()) return "每项任务必须提供files写入边界数组；只读任务用[]";
        for (const auto& file:t.at("files")) {
            if (!file.is_string()) return "files必须是相对路径字符串数组";
            auto path=plan_scope(file.get<std::string>());
            if (path.ends_with("/**")) path.resize(path.size()-3);
            if (path.empty() || path.starts_with('/') || path.find(':')!=path.npos || path.find_first_of("*?\r\n")!=path.npos || path==".." || path.starts_with("../") || path.ends_with("/..") || path.find("/../")!=path.npos)
                return "files只允许仓库内相对文件或目录/**，禁止绝对路径、越界及其他通配符";
        }
    }
    std::vector<int> colors(tasks.size(),0);
    std::function<bool(std::size_t)> cycle=[&](std::size_t i) {
        if (colors[i]==1) return true;if (colors[i]==2) return false;colors[i]=1;
        std::set<std::string> seen;
        for (const auto& d:tasks.at(i).at("dependsOn")) {
            if (!d.is_string()) return true;
            const auto name=d.get<std::string>();
            if (!names.contains(name) || !seen.insert(name).second || cycle(names.at(name))) return true;
        }
        colors[i]=2;return false;
    };
    for (std::size_t i=0;i<tasks.size();++i) if (cycle(i)) return "dependsOn存在未知名称、重复依赖、自依赖或循环";
    std::vector<std::vector<bool>> closure(tasks.size(),std::vector<bool>(tasks.size(),false));
    for (std::size_t i=0;i<tasks.size();++i) for (const auto& d:tasks.at(i).at("dependsOn")) closure[i][names.at(d.get<std::string>())]=true;
    for (std::size_t k=0;k<tasks.size();++k) for (std::size_t i=0;i<tasks.size();++i) for (std::size_t j=0;j<tasks.size();++j) closure[i][j]=closure[i][j] || (closure[i][k] && closure[k][j]);
    for (std::size_t a=0;a<tasks.size();++a) for (std::size_t b=a+1;b<tasks.size();++b)
        if (!closure[a][b] && !closure[b][a]) for (const auto& x:tasks.at(a).at("files")) for (const auto& y:tasks.at(b).at("files"))
            if (plan_scopes_overlap(x.get<std::string>(),y.get<std::string>())) return "并行任务写入边界重叠："+tasks.at(a).at("name").get<std::string>()+" ["+x.get<std::string>()+"] 与 "+tasks.at(b).at("name").get<std::string>()+" ["+y.get<std::string>()+"]；请分配文件所有权或声明真实依赖";
    return {};
}
// State is derived from the actual ledger. A developer finishing is not acceptance.
inline Json task_plan_readiness(const Json& tasks,const Json& states) {
    Json ready=Json::array(),blocked=Json::array();
    for (const auto& t:tasks) {
        const auto name=t.at("name").get<std::string>();
        if (states.value(name,"")!="") continue;
        Json waiting=Json::array();
        for (const auto& d:t.at("dependsOn")) {
            const auto state=states.value(d.get<std::string>(),"");
            if (state!="accepted" && state!="integrated") waiting.push_back(d);
        }
        if (waiting.empty()) ready.push_back(name);else blocked.push_back({{"name",name},{"waitingFor",waiting}});
    }
    return {{"readyTasks",ready},{"blockedTasks",blocked},{"planNeedsUpdate",false}};
}
}
