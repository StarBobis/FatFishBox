#pragma once
#include "cluster.h"
#include <string_view>
namespace fatfish {
// DSH's projection wire value wraps the durable goal in {goal, roundsStarted,...}.
// Older hosts may expose the durable goal directly. Return one canonical shape.
inline Json goal_snapshot(const Json& projection) {
    const auto path="/values/goal"_json_pointer;
    if (!projection.contains(path)) return nullptr;
    const auto& wire=projection.at(path);
    if (!wire.is_object()) return nullptr;
    const auto& goal=wire.contains("goal")?wire.at("goal"):wire;
    if (!goal.is_object()) return nullptr;
    for (const auto* key:{"id","phase","objective"})
        if (!goal.contains(key) || !goal.at(key).is_string()) return nullptr;
    if (goal.at("id").get_ref<const std::string&>().empty()) return nullptr;
    return goal;
}
// Match JavaScript String.trim (the DSH goal domain's only objective normalization).
// Do not collapse internal whitespace or accept a different objective.
inline std::string normalized_goal_objective(std::string_view text) {
    const auto wide=widen(std::string(text));
    const auto space=[](wchar_t ch) {
        return (ch>=0x0009 && ch<=0x000D) || ch==0x0020 || ch==0x00A0 || ch==0x1680 ||
            (ch>=0x2000 && ch<=0x200A) || ch==0x2028 || ch==0x2029 || ch==0x202F ||
            ch==0x205F || ch==0x3000 || ch==0xFEFF;
    };
    std::size_t first=0,last=wide.size();
    while (first<last && space(wide[first])) ++first;
    while (last>first && space(wide[last-1])) --last;
    return narrow(wide.substr(first,last-first));
}
inline bool goal_matches(const Json& goal,std::string_view objective) {
    return !goal.is_null() && goal.value("phase","")=="active" &&
        normalized_goal_objective(goal.at("objective").get_ref<const std::string&>())==normalized_goal_objective(objective);
}
}
