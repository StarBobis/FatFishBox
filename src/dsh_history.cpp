#include "dsh_history.h"
#include <windows.h>
#include <zstd.h>
#include <zstd_errors.h>
#include <array>
#include <fstream>
#include <memory>

namespace fatfish {
namespace {
constexpr std::size_t file_limit=128*1024*1024;
constexpr std::size_t line_limit=16*1024*1024;

fs::path environment_path(const wchar_t* name) {
    const auto size=GetEnvironmentVariableW(name,nullptr,0);
    if (!size) return {};
    std::wstring value(size,L'\0');
    const auto written=GetEnvironmentVariableW(name,value.data(),size);
    if (!written || written>=size) return {};
    value.resize(written);
    return value;
}

std::wstring project_key(const std::wstring& cwd) {
    constexpr std::wstring_view digits=L"0123456789ABCDEF";
    std::wstring result;
    bool separator=false;
    for (const auto ch:cwd) {
        if (ch==L'/' || ch==L'\\' || ch==L':') {
            if (!separator) result+=L'-';
            separator=true;
        } else {
            separator=false;
            if ((ch>=L'A' && ch<=L'Z') || (ch>=L'a' && ch<=L'z') || (ch>=L'0' && ch<=L'9') ||
                ch==L'.' || ch==L'_' || ch==L'-') result+=ch;
            else {
                result+=L'~';
                for (const auto shift:{12,8,4,0}) result+=digits[(ch>>shift)&15];
            }
        }
    }
    const auto first=result.find_first_not_of(L'-');
    return L"--"+(first==result.npos?std::wstring(L"root"):result.substr(first,251))+L"--";
}

std::string assistant_text(const Json& event) {
    const auto& data=event.at("data");
    const auto& message=data.contains("message")?data.at("message"):data;
    const auto content=message.value("content",Json());
    if (content.is_string()) return content.get<std::string>();
    std::string result;
    if (content.is_array()) for (const auto& part:content) {
        if (!part.is_object() || !part.contains("text") || !part.at("text").is_string()) continue;
        if (!result.empty()) result+='\n';
        result+=part.at("text").get_ref<const std::string&>();
    }
    return result;
}
}

fs::path dsh_history_file(const std::string& session,const std::string& workspace) {
    if (!session.starts_with("session-") ||
        session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")!=session.npos)
        return {};
    auto root=environment_path(L"FATFISHBOX_DSH_SESSIONS_DIR");
    if (root.empty()) root=environment_path(L"USERPROFILE")/L".dsh"/L"sessions";
    if (!root.is_absolute()) return {};
    const auto directory=root/(workspace.empty()?std::wstring(L"_no-cwd"):project_key(widen(workspace)))/path_from_utf8(session);
    for (const auto* name:{L"session.v4.jsonl.zstd",L"session.v4.jsonl"}) {
        std::error_code error;
        const auto file=directory/name;
        if (fs::is_regular_file(file,error)) return file;
    }
    return {};
}

std::optional<DshHistory> read_dsh_history(const fs::path& file,const std::string& session,
    std::int64_t after,std::optional<std::int64_t> through) {
    if (file.empty()) return {};
    try {
        std::ifstream input(file,std::ios::binary);
        if (!input) return {};
        bool header=false;
        std::int64_t cursor=-1;
        std::optional<DshHistory> result;
        std::string output,line;
        const auto consume=[&](std::string_view bytes) {
            while (!bytes.empty()) {
                const auto newline=bytes.find('\n');
                line.append(bytes.substr(0,newline));
                if (line.size()>line_limit) throw std::runtime_error("DSH 日志记录超出大小限制");
                if (newline==bytes.npos) break;
                bytes.remove_prefix(newline+1);
                const auto row=Json::parse(line);
                line.clear();
                if (!header) {
                    if (row.value("type","")!="session" || row.value("version",0)!=4 || row.value("id","")!=session)
                        throw std::runtime_error("DSH 日志标头不匹配");
                    header=true;
                    continue;
                }
                const auto seq=row.at("seq").get<std::int64_t>();
                if (through && seq>*through) continue;
                if (seq!=cursor+1) throw std::runtime_error("DSH 日志序号不连续");
                cursor=seq;
                if (seq<=after) continue;
                const auto type=row.value("type","");
                if (type=="turn/start") {
                    result.reset();
                    output.clear();
                } else if (type=="assistant/message" || type=="assistant/response") {
                    auto message=assistant_text(row);
                    if (!message.empty()) output=std::move(message);
                } else if (type=="turn/end") {
                    const auto& reason=row.at("data").at("reason");
                    const auto kind=reason.value("kind","");
                    std::string status;
                    if (kind=="completed") status="succeeded";
                    else if (kind=="aborted" || kind=="cancelled" || kind=="canceled") status="cancelled";
                    else if (kind=="interrupted" || kind=="forked") status="interrupted";
                    else if (kind=="blocked" || kind=="error" || kind=="max-tokens") status="failed";
                    if (status.empty()) result.reset();
                    else result=DshHistory{{status,seq,row.value("time",std::int64_t(0)),failure_details(reason)},seq,output};
                }
            }
        };
        std::array<char,65536> buffer{};
        if (file.extension()==L".zstd") {
            const auto size=fs::file_size(file);
            if (size>file_limit) return {};
            std::string compressed(static_cast<std::size_t>(size),'\0');
            if (!input.read(compressed.data(),static_cast<std::streamsize>(size))) return {};
            const std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)> decoder(ZSTD_createDCtx(),ZSTD_freeDCtx);
            if (!decoder || ZSTD_isError(ZSTD_DCtx_setParameter(decoder.get(),ZSTD_d_windowLogMax,27))) return {};
            std::size_t offset=0;
            while (offset<compressed.size() && (!through || cursor<*through)) {
                const auto remaining=compressed.size()-offset;
                const auto length=ZSTD_findFrameCompressedSize(compressed.data()+offset,remaining);
                if (ZSTD_isError(length)) {
                    if (ZSTD_getErrorCode(length)==ZSTD_error_srcSize_wrong) break;
                    return {};
                }
                ZSTD_inBuffer source{compressed.data()+offset,length,0};
                std::size_t pending;
                do {
                    const auto previous=source.pos;
                    ZSTD_outBuffer target{buffer.data(),buffer.size(),0};
                    pending=ZSTD_decompressStream(decoder.get(),&target,&source);
                    if (ZSTD_isError(pending) || (pending && !target.pos && source.pos==previous)) return {};
                    consume({buffer.data(),target.pos});
                } while (source.pos<source.size || pending);
                offset+=length;
            }
        } else {
            if (fs::file_size(file)>file_limit) return {};
            while (input && (!through || cursor<*through)) {
                input.read(buffer.data(),buffer.size());
                consume({buffer.data(),static_cast<std::size_t>(input.gcount())});
            }
            if (input.bad()) return {};
        }
        if (result && (!through || cursor==*through)) {
            result->cursor=cursor;
            return result;
        }
    } catch (const std::exception&) {}
    return {};
}
}
