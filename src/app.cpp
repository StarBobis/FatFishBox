#include "cluster.h"
#include "mcp.h"
#include <native_ui/button.h>
#include <native_ui/navigation_view.h>
#include <native_ui/renderer.h>
#include <native_ui/text_box.h>
#include <native_ui/title_bar.h>
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using namespace native_ui;
using fatfish::Cluster;
using fatfish::Json;
using fatfish::narrow;
using fatfish::widen;
constexpr UINT finished = WM_APP + 1;
constexpr std::array roles{"developer","reviewer","repair","support"};
constexpr std::array role_names{L"开发",L"评审",L"返修",L"协助"};
std::wstring title(const Json& object, const char* member) {
    return widen(object.value(member, ""));
}
std::wstring status(const Json& row) {
    const std::map<std::string,std::wstring> labels{
        {"starting",L"启动中"},{"queued",L"排队"},{"running",L"运行中"},{"waiting_input",L"等待输入"},
        {"succeeded",L"完成"},{"cancelled",L"取消"},{"failed",L"失败"},{"unknown",L"待同步"},{"interrupted",L"中断"}};
    auto found = labels.find(row.value("status",""));
    return found == labels.end() ? L"—" : found->second;
}
std::wstring fixed3(double value) {
    std::wostringstream text;
    text<<std::fixed<<std::setprecision(3)<<value;
    return text.str();
}
std::wstring compact_tokens(double value) {
    constexpr std::array<std::pair<double,const wchar_t*>,3> scales{{{1e9,L"B"},{1e6,L"M"},{1e3,L"K"}}};
    for (const auto& [size,suffix]: scales)
        if (value>=size*.9999995) return fixed3(value/size)+suffix;
    return fixed3(value);
}

class App {
public:
    HWND window{};
    ~App() { if (worker_.joinable()) worker_.join(); }
    void init() {
        model_.theme = Theme::dark;
        renderer_.initialize();
        redraw();
        reload();
    }
    void size(int width, int height) {
        if (!width || !height) return;
        renderer_.resize(static_cast<UINT>(width), static_cast<UINT>(height));
        const auto current = page_size();
        redraw();
        if (current != page_size() && width > 0 && height > 0 && page_=="sessions") {
            local_snapshot();
            redraw();
        }
    }
    void dpi(UINT value) { renderer_.set_dpi(value); redraw(); }
    void paint() { renderer_.paint(window, model_, layout_, Theme::dark); }
    void activate(bool active) {
        model_.active = active;
        InvalidateRect(window,nullptr,FALSE);
    }
    bool caption_control(POINT point) const {
        const auto* control = widget(model_,hit(point.x,point.y));
        return control && control->kind == WidgetKind::button && control->surface == "titlebar.titlebar";
    }
    bool in_caption(POINT point) const {
        return DpiScale(GetDpiForWindow(window)).dips(point.y) < model_.caption_height;
    }
    void hover(int x, int y) {
        if (model_.hovered != hit(x,y)) {
            model_.hovered = hit(x,y);
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
        TrackMouseEvent(&track);
    }
    void leave() { model_.hovered = none; InvalidateRect(window, nullptr, FALSE); }
    void down(int x, int y) {
        auto which = hit(x,y);
        auto* w = widget(model_,which);
        if (!w || !w->enabled || (w->kind != WidgetKind::textbox && w->kind != WidgetKind::button)) return;
        SetFocus(window);
        model_.focused = which;
        model_.pressed = which;
        model_.focus_visible = true;
        const DpiScale scale(GetDpiForWindow(window));
        if (w->kind == WidgetKind::textbox) {
            TextEditor::focus(model_.ui,w->value,which);
            TextBox::pointer(model_,layout_,which,float(scale.dips(x)),float(scale.dips(y)));
        }
        SetCapture(window);
        InvalidateRect(window,nullptr,FALSE);
    }
    void up(int x, int y) {
        const int pressed = model_.pressed;
        model_.pressed = none;
        if (GetCapture() == window) ReleaseCapture();
        if (pressed == hit(x,y)) click(pressed);
        InvalidateRect(window,nullptr,FALSE);
    }
    void lost_capture() { model_.pressed = none; InvalidateRect(window,nullptr,FALSE); }
    bool key(WPARAM pressed) {
        if (pressed == VK_TAB) {
            std::vector<int> controls;
            for (std::size_t i = 0; i < model_.ui.widgets.size(); ++i) {
                const auto& w = model_.ui.widgets[i];
                if (w.enabled && (w.kind == WidgetKind::textbox || w.kind == WidgetKind::button))
                    controls.push_back(widget_base + int(i));
            }
            if (controls.empty()) return true;
            auto current = std::find(controls.begin(),controls.end(),model_.focused);
            const int n = int(controls.size());
            const int step = (GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
            int index = current == controls.end() ? (step > 0 ? 0 : n - 1) : (int(current-controls.begin()) + n + step) % n;
            model_.focused = controls[index];
            model_.focus_visible = true;
            if (auto* box = widget(model_,model_.focused); box && box->kind == WidgetKind::textbox)
                TextEditor::focus(model_.ui,box->value,model_.focused);
            InvalidateRect(window,nullptr,FALSE);
            return true;
        }
        auto* box = widget(model_,model_.focused);
        if (!box || !box->enabled) return false;
        if (box->kind == WidgetKind::button && (pressed == VK_RETURN || pressed == VK_SPACE)) {
            click(model_.focused);
            return true;
        }
        if (box->kind != WidgetKind::textbox) return false;
        if (GetKeyState(VK_CONTROL) < 0 && pressed == 'V') {
            if (OpenClipboard(window)) {
                if (HANDLE item = GetClipboardData(CF_UNICODETEXT))
                    if (const auto* text = static_cast<const wchar_t*>(GlobalLock(item))) {
                        insert(text);
                        GlobalUnlock(item);
                    }
                CloseClipboard();
            }
            return true;
        }
        if (GetKeyState(VK_CONTROL) < 0 && pressed == 'A') {
            TextBox::select_all(model_,model_.focused);
            InvalidateRect(window,nullptr,FALSE);
            return true;
        }
        const std::map<WPARAM,std::string> keys{{VK_LEFT,"Left"},{VK_RIGHT,"Right"},{VK_HOME,"Home"},{VK_END,"End"},
            {VK_DELETE,"Delete"},{VK_BACK,"Backspace"}};
        auto found = keys.find(pressed);
        if (found == keys.end()) return false;
        if (TextBox::key(model_,layout_,found->second,GetKeyState(VK_SHIFT) < 0)) {
            fields_[box->binding] = box->value;
            InvalidateRect(window,nullptr,FALSE);
        }
        return true;
    }
    void type(wchar_t c) {
        if (c >= 32 || c == L'\r') {
            if (c == L'\r') c = L'\n';
            insert(std::wstring_view(&c,1));
        }
    }
    void receive() {
        if (worker_.joinable()) worker_.join();
        busy_ = false;
        if (error_.empty()) {
            notice_.clear();
            if (result_ && callback_) callback_(std::move(*result_));
        } else {
            notice_ = error_;
        }
        redraw();
    }
private:
    Cluster cluster_;
    SurfaceRenderer renderer_;
    WidgetModel model_;
    WidgetLayout layout_{1,1};
    TitleBar title_bar_;
    NavigationView navigation_;
    std::map<std::string,std::wstring> fields_;
    Json roster_ = Json::object(), settings_ = Json::object(), overview_ = Json::object(), editing_, selected_;
    std::vector<std::pair<bool,Json>> pools_;
    std::vector<Json> visible_sessions_;
    std::jthread worker_;
    std::optional<Json> result_;
    std::function<void(Json)> callback_;
    std::wstring notice_, error_, output_;
    std::string page_ = "sessions";
    int offset_ = 0, pool_page_ = 0, role_ = 0, effort_ = 0, balance_ = 0;
    bool busy_ = false, handoff_ = false, enabled_ = false, is_pool_ = false;
    bool navigation_open_ = true;
    constexpr static std::array balances{"least_loaded","priority","weighted"};
    constexpr static std::array balance_names{L"最少负载",L"优先级优先",L"按权重"};

    void insert(std::wstring_view text) {
        auto* box = widget(model_,model_.focused);
        if (!box || box->kind != WidgetKind::textbox || !box->enabled) return;
        if (TextEditor::insert(model_.ui,box->value,model_.focused,text,box->binding == "task",64000))
            fields_[box->binding] = box->value;
        InvalidateRect(window,nullptr,FALSE);
    }
    int hit(int x, int y) const {
        const DpiScale scale(GetDpiForWindow(window));
        return layout_.hit(float(scale.dips(x)),float(scale.dips(y)));
    }
    void job(std::function<Json()> work, std::function<void(Json)> after) {
        if (busy_) return;
        if (worker_.joinable()) worker_.join();
        busy_ = true;
        result_.reset();
        error_.clear();
        callback_ = std::move(after);
        notice_ = L"正在处理…";
        redraw();
        worker_ = std::jthread([this, work=std::move(work)] {
            try { result_ = work(); }
            catch (const std::exception& e) {
                try { error_ = widen(e.what()); } catch (...) { error_ = L"操作失败"; }
            }
            PostMessageW(window,finished,0,0);
        });
    }
    void call(std::string operation, Json input, std::function<void(Json)> after) {
        job([this, operation=std::move(operation), input=std::move(input)] { return cluster_.invoke(operation,input); },
            std::move(after));
    }
    void load_connection() {
        for (auto key : {"configPath","authPath","permission","preset"})
            fields_[key] = title(settings_,key);
        auto policy = settings_.value("balance","least_loaded");
        const auto current = std::find(balances.begin(),balances.end(),policy);
        balance_ = current == balances.end() ? 0 : int(current-balances.begin());
    }
    void local_snapshot() {
        try {
            overview_ = cluster_.invoke("cluster_overview");
            settings_ = cluster_.invoke("cluster_get_settings");
            if (page_ != "connection") {
                const auto policy=settings_.value("balance","least_loaded");
                const auto current=std::find(balances.begin(),balances.end(),policy);
                if (current!=balances.end()) balance_=int(current-balances.begin());
            }
            roster_ = cluster_.invoke("cluster_list_sessions",{{"offset",offset_},{"limit",page_size()},{"handoffOnly",handoff_}});
        } catch (const std::exception& e) { notice_ = widen(e.what()); }
    }
    void reload() { local_snapshot(); redraw(); }
    int page_size() const { return std::clamp(1+int((layout_.height-title_bar_.height()-90-41-312)/38),1,100); }
    void nav(std::string to) {
        page_ = std::move(to);
        model_.focused = none;
        model_.hovered = none;
        model_.pressed = none;
        notice_.clear();
        if (page_ == "connection") load_connection();
        redraw();
    }
    void panel(std::string key, Rect box, SurfaceVisual visual = SurfaceVisual::card) {
        Surface surface;
        surface.key = std::move(key);
        surface.kind = SurfaceKind::panel;
        surface.visual = visual;
        if (surface.key != "root") {
            box.top += title_bar_.height();
            box.bottom += title_bar_.height();
        }
        surface.bounds = box;
        surface.header_height = surface.horizontal_padding = surface.bottom_padding = 0;
        model_.ui.surfaces.push_back(std::move(surface));
    }
    Widget& add(std::string key, WidgetKind kind, std::wstring label, float x,float y,float w,float h, std::string surface="root") {
        Widget item;
        item.key = key;
        item.action = std::move(key);
        item.surface = std::move(surface);
        item.area = WidgetArea::absolute;
        y += title_bar_.height();
        item.bounds = {x,y,x+w,y+h};
        item.kind = kind;
        item.label = std::move(label);
        model_.ui.widgets.push_back(std::move(item));
        return model_.ui.widgets.back();
    }
    void label(std::wstring text,float x,float y,float w,float size=14,std::string surface="root") {
        add("",size >= 23 ? WidgetKind::heading : WidgetKind::label,std::move(text),x,y,w,std::max(24.f,size+10),std::move(surface)).font_size=size;
    }
    void button(std::string key,std::wstring name,float x,float y,float w,bool primary=false,bool allowed=true,std::string surface="root") {
        auto& item = add(std::move(key),WidgetKind::button,name,x,y,w,32,std::move(surface));
        Button(ButtonOptions{.text=std::move(name),.primary=primary,.enabled=allowed && !busy_,.width=w}).apply(item);
    }
    void field(std::string key,std::wstring name,float x,float y,float w,bool multiline=false) {
        label(std::move(name),x,y,w,13);
        auto& item = add("input."+key,WidgetKind::textbox,L"",x,y+26,w,multiline?105.f:32.f);
        item.binding = key;
        TextBox(TextBoxOptions{.text=fields_[key],.enabled=!busy_,.multiline=multiline,.accepts_return=multiline,.height=105}).apply(item);
    }
    void redraw() {
        if (!window) return;
        RECT rect{};
        GetClientRect(window,&rect);
        const DpiScale scale(GetDpiForWindow(window));
        const float w=float(scale.dips(rect.right)), client_height=float(scale.dips(rect.bottom));
        if (w < 100 || client_height < 100) return;
        const bool maximized = IsZoomed(window) != FALSE;
        title_bar_.configure({
            .title=L"肥鱼重工", .minimize_label=L"最小化",
            .maximize_label=maximized?L"还原":L"最大化", .close_label=L"关闭",
            .maximized=maximized, .pane_toggle=true, .pane_toggle_label=L"切换导航栏",
        });
        const float h = client_height-title_bar_.height();
        layout_=WidgetLayout(w,client_height);
        model_.ui.widgets.clear();
        model_.ui.surfaces.clear();
        panel("root",{0,0,w,client_height},SurfaceVisual::transparent);
        title_bar_.append(model_,w);
        const auto selected_page=page_=="pools" || page_=="edit" ? "pools" :
            page_=="connection" ? "connection" : "sessions";
        NavigationViewOptions navigation;
        navigation.id="sidebar";
        navigation.pane_expanded=navigation_open_;
        navigation.selected_id=selected_page;
        navigation.items={{"sessions",L"会话与交接",L"\uE8BD",!busy_,{}},
                          {"pools",L"账号与模型池",L"\uE716",!busy_,{}}};
        navigation.settings={"connection",L"连接与 MCP",L"\uE713",!busy_,{}};
        navigation.expanded_width=192;
        navigation.pane_toggle_visible=false;
        navigation.search_visible=false;
        navigation_.configure(std::move(navigation));
        navigation_.append(model_,w,client_height,title_bar_.height());
        const float x=navigation_.width()+24, right=w-24, width=right-x;
        const std::map<std::string,std::wstring> names{{"sessions",L"会话与交接"},{"pools",L"账号与模型池"},
            {"connection",L"连接与 MCP"},{"dispatch",L"派发会话"},{"detail",L"会话详情"},{"edit",L"调度配置"}};
        label(names.at(page_),x,24,width-245,29);
        if (!notice_.empty()) label(notice_,x,h-45,width,14);
        if (page_=="sessions") {
            button("sync",L"同步会话",right-231,27,106);
            button("new",L"派发会话",right-114,27,114,true);
            const std::array<std::pair<const char*,const wchar_t*>,3> counts{{
                {"active",L"进行中"}, {"handoffs",L"待交接"}, {"unread",L"未读"}
            }};
            const float cell=(width-24)/3;
            for (std::size_t i=0;i<counts.size();++i) {
                const float lx=x+float(i)*(cell+12);
                auto key="metric."+std::to_string(i);
                panel(key,{lx,90,lx+cell,177});
                label(counts[i].second,lx+14,98,cell-24,14,key);
                label(std::to_wstring(overview_.value(counts[i].first,0)),lx+14,124,cell-24,27,key);
            }
            const auto usage=overview_.value("usage",Json::object());
            const auto total=usage.value("total",Json::object());
            const auto input=total.value("uncachedInputTokens",0.0), read=total.value("cacheReadTokens",0.0);
            const auto output=total.value("outputTokens",0.0), write=total.value("cacheWriteTokens",0.0);
            const auto billed=input+read+write;
            label(L"输入 "+compact_tokens(input)+L"  ·  输出 "+compact_tokens(output)+L"  ·  缓存读取 "+compact_tokens(read)+
                L"  ·  命中 "+(billed>0?fixed3(read*100/billed)+L"%":L"—"),x+1,184,width*.59f,12);
            const auto hourly=usage.value("hourlyTokens",Json(nullptr));
            label(L"今日平均每小时  "+(hourly.is_number()?compact_tokens(hourly.get<double>())+L" Token/h":L"—")+
                (usage.value("missing",0)?L" · 部分":L""),x+width*.60f,184,width*.40f,12);
            panel("list",{x,222,right,h-56});
            button("handoff",handoff_?L"显示全部会话":L"只看待交接",x+13,233,120,false,true,"list");
            label(L"会话",x+18,282,width*.37f,13,"list");
            label(L"模型",x+width*.48f,282,width*.27f,13,"list");
            label(L"状态",right-122,282,106,13,"list");
            visible_sessions_.clear();
            float y=312;
            for (const auto& row : roster_.value("sessions",Json::array())) {
                if (y+41>h-90) break;
                auto key="row."+std::to_string(visible_sessions_.size());
                visible_sessions_.push_back(row);
                const auto provenance=row.value("readOnly",false)?L"Codex · ":L"";
                button(key,provenance+title(row,"title"),x+13,y,width*.43f,false,true,"list");
                label(title(row,"model"),x+width*.48f,y+2,width*.32f,14,"list");
                label(status(row),right-122,y+2,104,14,"list");
                y+=38;
            }
            if (visible_sessions_.empty()) label(L"当前范围内暂无会话",x+20,y+17,width-38,16,"list");
            if (overview_.value("codexSources",0)>0)
                label(L"Codex 登记快照 · "+std::to_wstring(overview_.value("codexSessions",0))+L" 个会话",
                    x+14,h-96,width-210,12,"list");
            else if (overview_.value("unreadableCodexSources",0)>0)
                label(L"Codex 会话登记暂不可读",x+14,h-96,width-210,12,"list");
            if (offset_>0) button("prev",L"上一页",right-180,h-94,77,false,true,"list");
            if (!roster_.value("nextOffset",Json(nullptr)).is_null()) button("next",L"下一页",right-91,h-94,77,false,true,"list");
        } else if (page_=="connection") {
            button("import",L"从 DSH 导入模型",right-158,23,158,true);
            const float form=std::min(690.f,width);
            field("configPath",L"DSH 配置文件（留空时使用 DSH_WORKERS_CONFIG）",x,86,form);
            field("authPath",L"DSH 本机授权文件（可从配置自动识别）",x,153,form);
            field("permission",L"权限预设",x,220,form*.48f);
            field("preset",L"Agent 预设",x+form*.52f,220,form*.48f);
            button("balance",std::wstring(L"分配策略：")+balance_names[balance_],x,292,206);
            button("save_connection",L"保存连接设置",x+221,292,132,true);
            label(L"原生 MCP  /  标准输入输出",x,362,width,21);
            label(L"FatFishBox.exe --mcp",x,402,width,14);
        } else if (page_=="pools") {
            button("import",L"从 DSH 导入模型",right-158,23,158,true);
            pools_.clear();
            for (const auto& account : settings_.value("accounts",Json::object())) pools_.emplace_back(false,account);
            for (const auto& pool : settings_.value("pools",Json::object())) pools_.emplace_back(true,pool);
            label(L"调度策略：" + std::wstring(balance_names[balance_]),x,74,width,14);
            panel("poolrows",{x,113,right,h-56});
            const int per_page=std::max(3,int((h-224)/42));
            float y=130;
            for (int i=pool_page_;i<int(pools_.size()) && i<pool_page_+per_page;++i) {
                const auto& [pool,row]=pools_[i];
                auto identity=pool?title(row,"provider"):title(row,"id");
                button("pool."+std::to_string(i),(pool?L"模型池  ":L"账号  ")+title(row,"name")+L" · "+identity,x+12,y,width*.50f,false,true,"poolrows");
                const auto line=(row.at("enabled").get<bool>()?L"启用":L"关闭")+std::wstring(L"  ·  并发 ")+std::to_wstring(row.value("concurrency",1));
                label(line,x+width*.53f,y+2,width*.24f,14,"poolrows");
                if (pool) label(row.value("effort","").empty()?L"DSH 默认":title(row,"effort"),right-126,y+2,113,14,"poolrows");
                y+=42;
            }
            if (pools_.empty()) label(L"导入后可设置账号、角色、并发和思考强度。",x+16,143,width-32,16,"poolrows");
            if (pool_page_>0) button("poolprev",L"上一页",right-180,h-94,77,false,true,"poolrows");
            if (pool_page_+per_page<int(pools_.size())) button("poolnext",L"下一页",right-92,h-94,78,false,true,"poolrows");
        } else if (page_=="edit" && editing_.is_object()) {
            button("nav.pools",L"返回模型池",right-112,23,112);
            label(title(editing_,"name")+L" · "+title(editing_,is_pool_?"provider":"id"),x,76,width,19);
            button("enabled",enabled_?L"当前启用":L"当前关闭",x,118,135,enabled_);
            field("concurrency",L"总并发",x,168,130);
            if (is_pool_) {
                field("priority",L"优先级（越小越优先）",x+156,168,168);
                field("weight",L"权重",x+346,168,120);
                auto efforts=editing_.at("efforts");
                auto value=effort_>0 && effort_<=int(efforts.size())?widen(efforts[effort_-1].get<std::string>()):L"DSH 默认";
                button("effort",L"思考强度："+value,x,242,208);
            }
            for (std::size_t i=0;i<roles.size();++i)
                field(roles[i],std::wstring(role_names[i])+L"并发",x+float(i)*125,300,108);
            button("save_pool",L"保存调度配置",x,380,145,true);
        } else if (page_=="dispatch") {
            button("nav.sessions",L"返回会话",right-106,23,106);
            field("taskTitle",L"标题",x,90,width);
            field("batch",L"批次 ID",x,158,width*.47f);
            button("role",std::wstring(L"角色：")+role_names[role_],x+width*.51f,182,149);
            field("workspace",L"独立工作区绝对路径",x,225,width);
            field("task",L"任务",x,290,width,true);
            button("send",L"派发任务",x,442,126,true);
        } else if (page_=="detail" && selected_.is_object()) {
            button("nav.sessions",L"返回会话",right-110,23,110);
            label(title(selected_,"title"),x,77,width,22);
            label(L"批次  "+title(selected_,"batch")+L"   ·   "+status(selected_),x,116,width);
            label(L"工作区  "+title(selected_,"workspace"),x,148,width);
            label(L"结果版本  "+title(selected_,"resultVersion"),x,181,width);
            const bool linked=selected_.contains("dshId");
            const bool receipt=!selected_.value("resultVersion","").empty();
            const bool legacy=selected_.value("readOnly",false);
            button("refresh_row",L"刷新状态",x,228,96,false,linked);
            button("read_row",L"读取结果",x+106,228,96,false,linked);
            if (legacy) {
                label(L"此会话来自 Codex。请在原插件中确认或取消。",x+215,230,width-215,13);
            } else {
                button("cancel_row",L"取消",x+212,228,76,false,linked&&!receipt);
                button("mark_read",L"标记已读",x+298,228,106,false,receipt&&!selected_.value("read",false));
                button("mark_done",L"确认处理",x+414,228,106,true,receipt&&!selected_.value("processed",false));
            }
            auto text=!output_.empty()?output_:title(selected_,"error");
            if (!text.empty()) {
                auto& box=add("output",WidgetKind::textbox,L"",x,286,width,h-348);
                TextBox(TextBoxOptions{.text=text,.read_only=true,.multiline=true,.height=h-348}).apply(box);
            }
        }
        append_widget_layout(layout_,model_);
        InvalidateRect(window,nullptr,FALSE);
    }
    Json paths() const {
        return {{"configPath",narrow(fields_.at("configPath"))},{"authPath",narrow(fields_.at("authPath"))}};
    }
    int number(const std::string& name) const {
        const auto& text=fields_.at(name);
        std::size_t length=0;
        const auto value=std::stoi(text,&length);
        if (length!=text.size()) throw std::runtime_error("请输入整数");
        return value;
    }
    void click(int control) {
        auto* w=widget(model_,control);
        if (!w || !w->enabled || w->kind!=WidgetKind::button) return;
        if (w->key=="window.minimize") { ShowWindow(window,SW_MINIMIZE); return; }
        if (w->key=="window.maximize") { ShowWindow(window,IsZoomed(window)?SW_RESTORE:SW_MAXIMIZE); return; }
        if (w->key=="window.close") { PostMessageW(window,WM_CLOSE,0,0); return; }
        if (w->key=="titlebar.pane-toggle") { navigation_open_=!navigation_open_; redraw(); return; }
        if (busy_) return;
        if (auto page=navigation_.item_id(w->key); !page.empty()) { nav(std::move(page)); return; }
        const auto name=w->action;
        try {
            if (name=="balance") {balance_=(balance_+1)%3;redraw();return;}
            if (name=="role") {role_=(role_+1)%4;redraw();return;}
            if (name=="enabled") {enabled_=!enabled_;redraw();return;}
            if (name=="effort") {effort_=(effort_+1)%(int(editing_.at("efforts").size())+1);redraw();return;}
            if (name=="handoff") {handoff_=!handoff_;offset_=0;reload();return;}
            if (name=="prev") {offset_=std::max(0,offset_-page_size());reload();return;}
            if (name=="next") {offset_=roster_["nextOffset"].get<int>();reload();return;}
            if (name=="poolprev" || name=="poolnext") {
                int step=std::max(3,int((layout_.height-224)/42));
                pool_page_=std::max(0,pool_page_+(name=="poolnext"?step:-step));redraw();return;
            }
            if (name=="new") {nav("dispatch");return;}
            if (name.starts_with("row.")) {selected_=visible_sessions_.at(std::stoul(name.substr(4)));output_.clear();nav("detail");return;}
            if (name.starts_with("pool.")) {
                auto [is_pool,row]=pools_.at(std::stoul(name.substr(5)));
                editing_=row;is_pool_=is_pool;enabled_=row.value("enabled",false);effort_=0;
                fields_["concurrency"]=std::to_wstring(row.value("concurrency",1));
                for (auto role:roles) fields_[role]=row.at("roles").contains(role)?std::to_wstring(row.at("roles").at(role).get<int>()):L"";
                if (is_pool) {
                    for (auto key:{"priority","weight"}) fields_[key]=std::to_wstring(row.value(key,0));
                    const auto& efforts=row.at("efforts");
                    auto it=std::find(efforts.begin(),efforts.end(),row.at("effort"));
                    if (it!=efforts.end()) effort_=1+int(std::distance(efforts.begin(),it));
                }
                nav("edit");return;
            }
            if (name=="sync") {
                call("cluster_refresh_sessions",Json::object(),[this](Json value) {
                    local_snapshot();
                    notice_=L"已读取 Codex 登记并同步 "+std::to_wstring(value.at("sessions").size())+L" 个本地会话";
                    if (!value.at("errors").empty()) notice_+=L" · 有会话待同步";
                });
            } else if (name=="import") {
                call("cluster_import_models",page_=="connection"?paths():Json::object(),[this](Json value) {
                    local_snapshot();nav("pools");notice_=L"已导入 "+std::to_wstring(value.value("accounts",0))+L" 个账号、"+std::to_wstring(value.value("models",0))+L" 个模型";
                });
            } else if (name=="save_connection") {
                Json input=paths();
                input.update(Json{{"revision",settings_.at("revision")},{"permission",narrow(fields_.at("permission"))},
                    {"preset",narrow(fields_.at("preset"))},{"balance",balances[balance_]}});
                call("cluster_configure",input,[this](Json value){settings_=std::move(value);local_snapshot();notice_=L"连接设置已保存";});
            } else if (name=="save_pool") {
                Json caps=Json::object();
                for (auto role:roles) if (!fields_.at(role).empty()) caps[role]=number(role);
                Json input{{"revision",settings_.at("revision")},{"id",editing_.at("id")},{"enabled",enabled_},
                    {"concurrency",number("concurrency")},{"roles",caps}};
                if (is_pool_) {
                    input["priority"]=number("priority");input["weight"]=number("weight");
                    input["effort"]=effort_?editing_.at("efforts").at(effort_-1):Json("");
                }
                call(is_pool_?"cluster_update_pool":"cluster_update_account",input,[this](Json value) {
                    settings_=std::move(value);nav("pools");notice_=L"调度配置已保存";
                });
            } else if (name=="send") {
                call("cluster_start_session",{{"title",narrow(fields_.at("taskTitle"))},{"batch",narrow(fields_.at("batch"))},
                    {"role",roles[role_]},{"workspace",narrow(fields_.at("workspace"))},{"task",narrow(fields_.at("task"))}},
                    [this](Json value){selected_=std::move(value);output_.clear();local_snapshot();nav("detail");});
            } else if (name.ends_with("_row") || name.starts_with("mark_")) {
                const std::map<std::string,std::string> operations{{"refresh_row","cluster_refresh_session"},{"cancel_row","cluster_cancel_session"},
                    {"read_row","cluster_read_result"},{"mark_read","cluster_mark_receipt"},{"mark_done","cluster_mark_receipt"}};
                Json input{{"sessionId",selected_.at("id")}};
                if (name.starts_with("mark_")) input.update(Json{{"resultVersion",selected_.at("resultVersion")},{"processed",name=="mark_done"}});
                call(operations.at(name),input,[this,name](Json value) {
                    if (name=="read_row") {
                        output_.clear();
                        for (const auto& message:value.at("messages")) output_+=title(message,"text")+L"\r\n\r\n";
                        if (output_.empty()) output_=L"暂无输出";
                    } else {selected_=std::move(value);local_snapshot();}
                });
            }
        } catch (const std::exception& error) {notice_=widen(error.what());redraw();}
    }
};

LRESULT CALLBACK dispatch_window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (msg==WM_NCCREATE) {
        app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->window=hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd,msg,wp,lp);
    switch (msg) {
    case WM_CREATE: app->init();return 0;
    case WM_SIZE: app->size(LOWORD(lp),HIWORD(lp));return 0;
    case WM_ACTIVATE: app->activate(LOWORD(wp)!=WA_INACTIVE);break;
    case WM_NCCALCSIZE:
        if (wp) {
            if (IsZoomed(hwnd)) {
                auto& bounds=reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromRect(&bounds,MONITOR_DEFAULTTONEAREST),&monitor))
                    bounds=monitor.rcWork;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd,&point);
        const bool control=app->caption_control(point);
        if (!IsZoomed(hwnd)) {
            RECT client{};
            GetClientRect(hwnd,&client);
            const UINT dpi=GetDpiForWindow(hwnd);
            const int border=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
            const bool left=point.x<border, right=point.x>=client.right-border;
            const bool top=point.y<border, bottom=point.y>=client.bottom-border;
            if (bottom) return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
            if (!control) {
                if (top) return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
            }
        }
        return !control && app->in_caption(point)?HTCAPTION:HTCLIENT;
    }
    case WM_DPICHANGED: {
        auto* bounds=reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOACTIVATE|SWP_NOZORDER);
        app->dpi(HIWORD(wp));return 0;
    }
    case WM_GETMINMAXINFO: {
        DefWindowProcW(hwnd,msg,wp,lp);
        auto* info=reinterpret_cast<MINMAXINFO*>(lp);
        const auto scale=GetDpiForWindow(hwnd);
        info->ptMinTrackSize={MulDiv(938,scale,96),MulDiv(622,scale,96)};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)) {
            info->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};
            info->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT state{};BeginPaint(hwnd,&state);app->paint();EndPaint(hwnd,&state);return 0;
    }
    case WM_MOUSEMOVE: app->hover(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_MOUSELEAVE: app->leave();return 0;
    case WM_LBUTTONDOWN: app->down(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_LBUTTONUP: app->up(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_CAPTURECHANGED: app->lost_capture();return 0;
    case WM_KEYDOWN: if (app->key(wp)) return 0;break;
    case WM_CHAR: app->type(static_cast<wchar_t>(wp));return 0;
    case finished: app->receive();return 0;
    case WM_DESTROY: PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc{};
    if (auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc)) {
        const bool mcp=argc==2 && std::wstring_view(argv[1])==L"--mcp";
        LocalFree(argv);
        if (mcp) return fatfish::run_mcp_stdio();
    }
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        App app;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance=instance;wc.lpfnWndProc=dispatch_window;wc.lpszClassName=L"FatFishBoxWindow";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        if (!RegisterClassExW(&wc)) throw std::runtime_error("窗口类注册失败");
        const auto hwnd=CreateWindowExW(0,wc.lpszClassName,L"肥鱼重工",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,1200,800,nullptr,nullptr,instance,&app);
        if (!hwnd) throw std::runtime_error("窗口创建失败");
        SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        ShowWindow(hwnd,show);
        MSG message{};
        while (GetMessageW(&message,nullptr,0,0)>0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } catch (const std::exception& error) {
        MessageBoxW(nullptr,widen(error.what()).c_str(),L"肥鱼重工",MB_ICONERROR);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}
