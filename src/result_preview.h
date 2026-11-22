#pragma once
#include <string>
#include <string_view>
namespace fatfish {
inline std::wstring result_preview(std::wstring_view text) {
    constexpr std::size_t limit=65536;
    if (text.size()<=limit) return std::wstring(text);
    constexpr std::wstring_view suffix=L"\r\n\r\n[结果较长，此处显示前段；“复制结果/复制原因”可取得完整内容。]";
    auto length=limit-suffix.size();
    if (text[length-1]>=0xD800 && text[length-1]<=0xDBFF) --length;
    return std::wstring(text.substr(0,length))+std::wstring(suffix);
}
}
