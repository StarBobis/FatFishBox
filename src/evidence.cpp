#include "evidence.h"
#include "handoff_artifacts.h"
#include "state_file.h"
#include "dsh.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <ranges>
#include <array>
#include <cwctype>
#include <fstream>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fatfish {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() {if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
struct Hash {
    BCRYPT_HASH_HANDLE value{};
    Hash() {if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&value,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("无法开始证据哈希");}
    ~Hash() {if (value) BCryptDestroyHash(value);}
    void add(const char* bytes,DWORD size) {if (BCryptHashData(value,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes)),size,0)<0) throw std::runtime_error("证据哈希失败");}
    std::string finish() {
        std::array<unsigned char,32> bytes{};
        if (BCryptFinishHash(value,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0) throw std::runtime_error("无法完成证据哈希");
        constexpr char hex[]="0123456789abcdef";
        std::string result;
        for (const auto byte:bytes) {result+=hex[byte>>4];result+=hex[byte&15];}
        return result;
    }
};
std::string hash_file(const fs::path& path,std::optional<std::uint64_t> expected={}) {
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    if (file.value==INVALID_HANDLE_VALUE) throw std::runtime_error("无法读取已保存的证据对象");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value,&size) || size.QuadPart<0 || size.QuadPart>512LL*1024*1024 ||
        (expected && static_cast<std::uint64_t>(size.QuadPart)!=*expected)) throw std::runtime_error("证据对象大小不匹配或超过上限");
    Hash hash;std::array<char,65536> bytes{};
    for (;;) {DWORD read=0;if (!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)) throw std::runtime_error("证据对象读取失败");if (!read) break;hash.add(bytes.data(),read);}
    return hash.finish();
}
bool within(const fs::path& root,const fs::path& path) {
    if (root.empty()) return false;
    std::error_code error;
    const auto base=fs::weakly_canonical(root,error);if (error) return false;
    const auto target=fs::weakly_canonical(path,error);if (error) return false;
    auto left=base.begin(),right=target.begin();
    for (;left!=base.end();++left,++right) if (right==target.end() || CompareStringOrdinal(left->c_str(),-1,right->c_str(),-1,TRUE)!=CSTR_EQUAL) return false;
    return true;
}
std::string kind_for(const fs::path& path) {
    auto extension=path.extension().wstring();for(auto& ch:extension)ch=static_cast<wchar_t>(towlower(ch));
    if (extension==L".exe" || extension==L".dll") return "executable";
    if (extension==L".png" || extension==L".jpg" || extension==L".jpeg" || extension==L".webp") return "screenshot";
    if (path.filename()==L"CMakeCache.txt") return "build_config";
    if (extension==L".log" || extension==L".xml" || extension==L".trx") return "test_log";
    return "other";
}
Json copy_blob(const fs::path& data,const fs::path& path,int& reused,std::uint64_t& remaining,const fs::path& allowed_root={}) {
    Handle input{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    if (input.value==INVALID_HANDLE_VALUE) return {{"state","unavailable"},{"reason","file_open_"+std::to_string(GetLastError())}};
    std::array<wchar_t,32768> resolved{};
    const auto length=GetFinalPathNameByHandleW(input.value,resolved.data(),static_cast<DWORD>(resolved.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    std::wstring observed;
    if (length && length<resolved.size()) {
        observed.assign(resolved.data(),length);
        if (observed.starts_with(LR"(\\?\UNC\)")) observed=LR"(\\)"+observed.substr(8);
        else if (observed.starts_with(LR"(\\?\)")) observed.erase(0,4);
    }
    if (!allowed_root.empty() && (observed.empty() || !within(allowed_root,fs::path(observed))))
        return {{"state","unavailable"},{"reason","resolved_path_outside_workspace"}};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.value,&size) || size.QuadPart<0 || size.QuadPart>512LL*1024*1024)
        return {{"state","unavailable"},{"reason","file_size_limit_512MiB"}};
    if (static_cast<std::uint64_t>(size.QuadPart)>remaining) return {{"state","unavailable"},{"reason","capture_total_limit_512MiB"}};
    const auto folder=data/L"evidence"/L"blobs";
    fs::create_directories(folder);
    const auto temp=folder/path_from_utf8("capture-"+id()+".tmp");
    try {
        std::ofstream output(temp,std::ios::binary);
        if (!output) throw std::runtime_error("无法暂存证据");
        Hash hash;std::array<char,65536> buffer{};std::uint64_t total=0;
        for (;;) {
            DWORD read=0;
            if (!ReadFile(input.value,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)) throw std::runtime_error("证据读取失败");
            if (!read) break;
            total+=read;
            if (total>512ULL*1024*1024) throw std::runtime_error("证据超过大小上限");
            hash.add(buffer.data(),read);output.write(buffer.data(),read);
        }
        output.close();if (!output || total!=static_cast<std::uint64_t>(size.QuadPart)) throw std::runtime_error("证据未完整保存");
        const auto digest=hash.finish();
        const auto file=folder/path_from_utf8(digest);
        const ExclusiveStateLock lock(folder/path_from_utf8(digest+".lock"));
        bool same=false;
        if (fs::exists(file)) {try {same=hash_file(file,total)==digest;}catch(const std::exception&) {}}
        if (same) {fs::remove(temp);++reused;}
        else if (!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("无法提交证据对象");
        remaining-=total;
        return {{"state","captured"},{"sha256",digest},{"bytes",total},{"blobRef","sha256:"+digest},{"blobPath",utf8(file)},{"observedPath",observed.empty()?Json():Json(narrow(observed))}};
    } catch (...) {std::error_code ignored;fs::remove(temp,ignored);throw;}
}
std::wstring quote_arg(const std::wstring& value) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for (const auto ch:value) {
        if (ch==L'\\') {++slashes;continue;}
        if (ch==L'\"') {result.append(slashes*2+1,L'\\');result+=ch;}
        else {result.append(slashes,L'\\');result+=ch;}
        slashes=0;
    }
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
struct CommandResult {std::string output;DWORD exit=1;bool complete=false;};
CommandResult git(const fs::path& workspace,const std::vector<std::wstring>& arguments) {
    std::array<wchar_t,32768> program{};
    const auto length=SearchPathW(nullptr,L"git.exe",nullptr,static_cast<DWORD>(program.size()),program.data(),nullptr);
    if (!length || length>=program.size()) return {};
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE read{},write{};if (!CreatePipe(&read,&write,&security,0)) return {};
    Handle reader{read},writer{write};SetHandleInformation(reader.value,HANDLE_FLAG_INHERIT,0);
    Handle null_input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr)};
    if (null_input.value==INVALID_HANDLE_VALUE) return {};
    std::wstring command=quote_arg(program.data())+L" --no-optional-locks -c core.fsmonitor=false -c core.untrackedCache=false -C "+quote_arg(workspace.wstring());
    for (const auto& arg:arguments) command+=L" "+quote_arg(arg);
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput=null_input.value;start.StartupInfo.hStdOutput=writer.value;start.StartupInfo.hStdError=writer.value;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> attributes(bytes);
    start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&bytes)) return {};
    const HANDLE inherited[]{writer.value,null_input.value};
    if (!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(inherited),sizeof(inherited),nullptr,nullptr)) {
        DeleteProcThreadAttributeList(start.lpAttributeList);return {};
    }
    PROCESS_INFORMATION process{};
    const auto launched=CreateProcessW(program.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
        nullptr,workspace.c_str(),&start.StartupInfo,&process);
    DeleteProcThreadAttributeList(start.lpAttributeList);
    if (!launched) return {};
    Handle process_handle{process.hProcess},thread{process.hThread};CloseHandle(writer.value);writer.value=INVALID_HANDLE_VALUE;
    CommandResult result;const auto deadline=GetTickCount64()+4000;
    bool truncated=false;
    for (;;) {
        if (GetTickCount64()>=deadline) {TerminateProcess(process_handle.value,1);WaitForSingleObject(process_handle.value,1000);break;}
        DWORD available=0;
        if (PeekNamedPipe(reader.value,nullptr,0,nullptr,&available,nullptr) && available) {
            std::array<char,8192> buffer{};DWORD count=0;
            if (ReadFile(reader.value,buffer.data(),std::min<DWORD>(available,static_cast<DWORD>(buffer.size())),&count,nullptr) && count) {
                if (result.output.size()+count<=256*1024) result.output.append(buffer.data(),count);else truncated=true;
                continue;
            }
        }
        if (WaitForSingleObject(process_handle.value,0)==WAIT_OBJECT_0) {
            DWORD remaining=0;
            if (PeekNamedPipe(reader.value,nullptr,0,nullptr,&remaining,nullptr) && remaining) continue;
            GetExitCodeProcess(process_handle.value,&result.exit);result.complete=!truncated;break;
        }
        if (GetTickCount64()>=deadline) {TerminateProcess(process_handle.value,1);WaitForSingleObject(process_handle.value,1000);break;}
        Sleep(5);
    }
    return result;
}
Json git_identity(const fs::path& workspace) {
    Json result{{"workspace",utf8(workspace)},{"source","unavailable"},{"sourceBinding","capture_time_only"}};
    if (workspace.empty() || !workspace.is_absolute() || !fs::is_directory(workspace)) return result;
    auto ancestor=workspace;bool found=false;
    for (int depth=0;depth<32;++depth) {
        if (fs::exists(ancestor/L".git")) {found=true;break;}
        if (ancestor==ancestor.root_path()) break;
        ancestor=ancestor.parent_path();
    }
    if (!found) {result["reason"]="not_git_workspace";return result;}
    const auto head=git(workspace,{L"rev-parse",L"--verify",L"HEAD"});
    if (!head.complete || head.exit!=0) {result["reason"]="git_head_unavailable";return result;}
    auto sha=head.output;
    while (!sha.empty() && (sha.back()=='\n' || sha.back()=='\r')) sha.pop_back();
    if ((sha.size()!=40 && sha.size()!=64) || sha.find_first_not_of("0123456789abcdefABCDEF")!=sha.npos) return result;
    result["source"]="git_observed";result["head"]=sha;
    const auto status=git(workspace,{L"status",L"--porcelain=v1",L"--untracked-files=normal"});
    result["worktreeClean"]=status.complete && status.exit==0?Json(status.output.empty()):Json();
    if (status.complete && status.exit==0) result["statusSha256"]=sha256(status.output);
    return result;
}
Json safe_git_identity(const fs::path& workspace) {
    try {return git_identity(workspace);}
    catch (const std::exception&) {return {{"workspace",utf8(workspace)},{"source","unavailable"},{"reason","git_metadata_unavailable"},{"sourceBinding","capture_time_only"}};}
}
std::string manifest_key(std::string reference) {
    for (const auto* prefix:{"fatfish://checks/","fatfish://evidence/"}) if (reference.starts_with(prefix)) {reference.erase(0,std::char_traits<char>::length(prefix));break;}
    if (reference.size()!=64 || reference.find_first_not_of("0123456789abcdef")!=reference.npos) throw std::runtime_error("无效的证据清单 ID");
    return reference;
}
std::string store_manifest(const fs::path& data,const std::string& queue,const Json& value) {
    const auto digest=sha256(value.dump());
    const auto folder=data/L"evidence"/L"manifests"/path_from_utf8(queue);
    fs::create_directories(folder);const auto file=folder/path_from_utf8(digest+".json");
    const ExclusiveStateLock lock(folder/path_from_utf8(digest+".lock"));
    if (!fs::exists(file) || read_json(file,4*1024*1024)!=value) write_state_file(file,value);
    return std::string(value.at("kind")=="checks"?"fatfish://checks/":"fatfish://evidence/")+digest;
}
fs::path attachment_file(const fs::path& data,const std::string& queue,const Json& row) {
    return data/L"evidence"/L"results"/path_from_utf8(queue)/path_from_utf8(sha256(row.at("id").get<std::string>()+"\n"+row.value("resultVersion",""))+".json");
}
}
Json capture_evidence(const fs::path& data,const std::string& queue,const Json& row,const Json& delivery,const Json& options) {
    const auto workspace=path_from_utf8(row.value("workspace",""));
    auto source=safe_git_identity(workspace);
    const auto expected=delivery.value("candidateSha",Json());
    source["expectedCandidateSha"]=expected;
    source["matchesExpectedCandidate"]=source.contains("head") && expected.is_string()?
        Json(_stricmp(source.at("head").get_ref<const std::string&>().c_str(),expected.get_ref<const std::string&>().c_str())==0):Json();
    Json candidates=Json::array();
    if (options.contains("files")) for (const auto& file:options.at("files")) {
        if (file.is_string()) candidates.push_back({{"path",file},{"origin","caller_selected"},{"explicit",true}});
        else candidates.push_back({{"path",file.at("path")},{"kind",file.value("kind",kind_for(path_from_utf8(file.at("path").get<std::string>())))},
            {"origin","caller_selected"},{"explicit",true}});
    }
    for (const auto& path:delivery.value("evidencePaths",Json::array())) candidates.push_back({{"path",path},{"origin","agent_reported"},{"explicit",false}});
    const auto tests=delivery.value("tests",Json::array());
    for (const auto& test:tests) if (test.contains("logPath") && test.at("logPath").is_string())
        candidates.push_back({{"path",test.at("logPath")},{"kind","test_log"},{"origin","reported_test_log"},{"explicit",false}});
    if (!workspace.empty()) for (const auto* name:{L"build/CMakeCache.txt",L"build/Testing/Temporary/LastTest.log"}) {
        const auto file=workspace/name;std::error_code error;
        if (fs::is_regular_file(file,error)) candidates.push_back({{"path",utf8(file)},{"origin","automatic_build_file"},{"explicit",false}});
    }
    const auto corrections=options.value("corrections",Json::array());
    if(!corrections.is_array() || corrections.size()>128) throw std::runtime_error("corrections must be an array of at most 128 entries");
    std::map<std::string,Json> corrected;
    for(const auto& correction:corrections) {
        const auto reported=correction.at("reportedPath").get<std::string>(),replacement=correction.at("replacementPath").get<std::string>(),reason=correction.at("reason").get<std::string>();
        if(!path_from_utf8(reported).is_absolute() || !path_from_utf8(replacement).is_absolute() || reason.empty()) throw std::runtime_error("Evidence correction requires absolute reportedPath/replacementPath and a nonempty reason");
        if(!std::ranges::any_of(candidates,[&](const Json& file){return file.at("path")==reported;})) throw std::runtime_error("reportedPath is not declared by this exact result: "+reported);
        if(!corrected.emplace(reported,correction).second) throw std::runtime_error("Duplicate evidence correction: "+reported);
    }
    Json files=Json::array(),build=Json::object();std::set<std::wstring> seen;
    int reused=0,unavailable=0;bool truncated=false;
    std::uint64_t remaining=512ULL*1024*1024;
    for (auto entry:candidates) {
        if (files.size()>=128) {truncated=true;break;}
        const auto reported=entry.at("path").get<std::string>();
        auto normalized=path_from_utf8(reported).lexically_normal().wstring();for(auto& ch:normalized)ch=static_cast<wchar_t>(towlower(ch));
        if (!seen.insert(normalized).second) continue;
        if(corrected.contains(reported)) {
            const auto& correction=corrected.at(reported);entry["reportedPath"]=reported;entry["path"]=correction.at("replacementPath");entry["correctionReason"]=correction.at("reason");entry["origin"]="explicit_correction";entry.erase("kind");
        }
        const auto path=path_from_utf8(entry.at("path").get<std::string>());
        const auto kind=entry.value("kind",kind_for(path));
        const auto explicitly_selected=entry.value("explicit",false);entry.erase("explicit");entry["kind"]=kind;
        if (!path.is_absolute()) entry.update({{"state","unavailable"},{"reason","absolute_path_required"}});
        else if (!explicitly_selected && !within(workspace,path)) entry.update({{"state","unavailable"},{"reason","outside_workspace_select_explicitly"}});
        else {
            try {entry.update(copy_blob(data,path,reused,remaining,explicitly_selected?fs::path():workspace));}
            catch (const std::exception& error) {entry.update({{"state","unavailable"},{"reason",error.what()}});}
        }
        if (entry.at("state")!="captured") ++unavailable;
        if (entry.at("state")=="captured" && kind=="build_config" && path.filename()==L"CMakeCache.txt" && entry.at("bytes").get<std::uint64_t>()<=4*1024*1024) {
            std::ifstream cache(path_from_utf8(entry.at("blobPath").get<std::string>()));
            for (std::string line;std::getline(cache,line);) {
                if (!line.empty() && line.back()=='\r') line.pop_back();
                const auto type=line.find(':'),equal=line.find('=');
                if (type==line.npos || equal==line.npos || type>equal) continue;
                const auto key=line.substr(0,type);
                if (key=="CMAKE_BUILD_TYPE" || key=="CMAKE_CONFIGURATION_TYPES" || key=="CMAKE_GENERATOR" || key=="CMAKE_HOME_DIRECTORY" ||
                    key=="CMAKE_PROJECT_NAME" || key=="CMAKE_CXX_COMPILER" || key=="CMAKE_CXX_COMPILER_ID" || key=="CMAKE_CXX_COMPILER_VERSION")
                    build[key]=line.substr(equal+1);
            }
            build["cacheSource"]="captured_cmake_cache";build["cacheBlobRef"]=entry.at("blobRef");
        }
        files.push_back(entry);
    }
    if (options.contains("configuration")) {build["requestedConfiguration"]=options.at("configuration");build["configurationSource"]="caller_reported";}
    const auto after=safe_git_identity(workspace);
    source["changedDuringCapture"]=source.value("head",Json())!=after.value("head",Json()) || source.value("statusSha256",Json())!=after.value("statusSha256",Json());
    Json checks_tests=Json::array();
    for (auto test:tests) {
        test["source"]=options.value("testSource",std::string("agent_reported"));
        if (test.contains("logPath")) for (const auto& file:files)
            if (file.at("path")==test.at("logPath") && file.at("state")=="captured") test["logEvidenceRef"]=file.at("blobRef");
        checks_tests.push_back(test);
    }
    const auto identity=Json{{"schemaVersion",1},{"queueId",queue},{"workItemId",row.value("workItemId",row.at("id").get<std::string>())},
        {"sessionId",row.at("id")},{"resultVersion",row.value("resultVersion","")},{"candidateSha",expected},
        {"baseSha",delivery.value("baseSha",row.value("baseSha",Json()))}};
    auto checks=identity;
    checks.update({{"kind","checks"},{"source",source},{"build",build},{"tests",checks_tests},
        {"testExecutionVerified",false},{"sourceBinding","capture_time_only"}});
    const auto checks_ref=store_manifest(data,queue,checks);
    auto manifest=identity;
    manifest.update({{"kind","evidence"},{"checksManifest",checks_ref},{"source",source},{"build",build},{"files",files},
        {"filesTruncated",truncated},{"fileCorrections",corrections},{"sourceBinding","capture_time_only"}});
    const auto evidence_ref=store_manifest(data,queue,manifest);
    return {{"checksManifest",checks_ref},{"evidenceManifest",evidence_ref},{"sourceSha",source.value("head",Json())},
        {"matchesExpectedCandidate",source.at("matchesExpectedCandidate")},{"worktreeClean",source.value("worktreeClean",Json())},{"sourceChangedDuringCapture",source.at("changedDuringCapture")},
        {"fileCount",files.size()},{"testCount",tests.size()},{"unavailableFiles",unavailable},{"reusedBlobs",reused},
        {"filesTruncated",truncated},{"fileCorrections",corrections},{"capturedAt",now()},{"testExecutionVerified",false},{"sourceBinding","capture_time_only"}};
}
Json read_evidence_manifest(const fs::path& data,const std::string& queue,const std::string& reference) {
    const auto key=manifest_key(reference);
    const auto file=data/L"evidence"/L"manifests"/path_from_utf8(queue)/path_from_utf8(key+".json");
    const auto value=read_json(file,4*1024*1024);
    if ((reference.starts_with("fatfish://checks/") && value.at("kind")!="checks") ||
        (reference.starts_with("fatfish://evidence/") && value.at("kind")!="evidence")) throw std::runtime_error("证据清单类型不匹配");
    if (value.at("queueId")!=queue || sha256(value.dump())!=key) throw std::runtime_error("证据清单身份或内容哈希不匹配");
    auto result=value;result["manifestId"]=reference;return result;
}
Json evidence_manifest_page(const fs::path& data,const Json& manifest,const Json& input) {
    const auto offset=input.value("offset",0),limit=input.value("limit",32);
    if (offset<0 || limit<1 || limit>100) throw std::runtime_error("offset 须非负，limit 须为 1 至 100");
    auto result=manifest;
    const auto key=result.at("kind")=="checks"?"tests":"files";
    const auto& values=manifest.at(key);
    const auto end=std::min(values.size(),static_cast<std::size_t>(offset)+static_cast<std::size_t>(limit));
    result[key]=Json::array();
    for (auto i=static_cast<std::size_t>(offset);i<end;++i) result[key].push_back(values.at(i));
    if (result.at("kind")=="evidence") for (auto& entry:result.at("files")) if (entry.value("state","")=="captured") {
        try {
            const auto digest=manifest_key(entry.at("sha256").get<std::string>());
            const auto same=hash_file(data/L"evidence"/L"blobs"/path_from_utf8(digest),entry.at("bytes").get<std::uint64_t>())==digest;
            entry["blobVerified"]=same;entry["blobState"]=same?"verified":"hash_mismatch";
        } catch (const std::exception&) {entry["blobVerified"]=false;entry["blobState"]="unavailable";}
    }
    result["total"]=values.size();result["nextOffset"]=end<values.size()?Json(end):Json();
    return result;
}
void remember_result_evidence(const fs::path& data,const std::string& queue,const Json& row,const Json& summary) {
    if (row.value("resultVersion","").empty()) return;
    const auto file=attachment_file(data,queue,row);fs::create_directories(file.parent_path());
    const ExclusiveStateLock lock(file.wstring()+L".lock");
    Json saved={{"queueId",queue},{"sessionId",row.at("id")},{"resultVersion",row.at("resultVersion")},{"history",Json::array()}};
    if (fs::exists(file)) saved=read_json(file,4*1024*1024);
    if (saved.contains("current") && saved.at("current").at("evidenceManifest")!=summary.at("evidenceManifest")) saved["history"].push_back(saved.at("current"));
    saved["current"]=summary;write_state_file(file,saved);
}
Json attach_result_evidence(const fs::path&,const std::string&,const Json& row,Json report) {
    if (report.value("resultVersion","").empty()) return report;
    if (!report.contains("delivery") || !report.at("delivery").is_object())
        report["delivery"]={{"status",report.contains("failure")?"failed":"missing"},{"source","unavailable"}};
    auto& normalized=report.at("delivery");
    normalized["workItemId"]=row.value("workItemId",row.at("id").get<std::string>());
    if (!normalized.contains("candidateSha")) normalized["candidateSha"]=nullptr;
    if (!normalized.contains("baseSha") || normalized.at("baseSha").is_null()) {
        normalized["baseSha"]=row.value("baseSha",Json());
        if (row.contains("baseSha")) normalized["baseShaSource"]="dispatch_record";
    }
    if (!normalized.contains("status")) normalized["status"]="reported";
    if (!normalized.contains("issues")) normalized["issues"]=Json::array();
    if (!normalized.contains("tests")) normalized["tests"]=Json::array();
    if (!normalized.contains("evidencePaths")) normalized["evidencePaths"]=Json::array();
    if (!normalized.contains("checksManifest")) normalized["checksManifest"]=nullptr;
    if (!normalized.contains("evidenceManifest")) normalized["evidenceManifest"]=nullptr;
    if (!normalized.contains("nextAction")) normalized["nextAction"]="inspect_result";
    // Reading a task result must never capture files, hash artifacts, inspect Git,
    // create manifests, or trigger a proof/audit workflow.
    return report;
}
}
