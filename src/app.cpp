#include "cluster.h"
#include "mcp.h"
#include "resource.h"
#include <native_ui/button.h>
#include <native_ui/combo_box.h>
#include <native_ui/image.h>
#include <native_ui/navigation_view.h>
#include <native_ui/renderer.h>
#include <native_ui/scroll_bar.h>
#include <native_ui/text_block.h>
#include <native_ui/text_box.h>
#include <native_ui/theme_resources.h>
#include <native_ui/title_bar.h>
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace native_ui;
using fatfish::Cluster;
using fatfish::Json;
using fatfish::narrow;
using fatfish::widen;
constexpr UINT finished = WM_APP + 1;
constexpr int minimum_width=740, minimum_height=520;
constexpr float row_height=34;
enum class List { running, pending, pools };
struct ListViewport {
    std::string key;
    Rect bounds{};
    double offset=0, maximum=0;
};
constexpr std::array roles{"developer","reviewer","repair","support"};
constexpr std::array role_names{L"开发",L"评审",L"返修",L"协助"};
const std::vector<Choice> balance_options{{"least_loaded",L"最少负载"},{"priority",L"优先级优先"},{"weighted",L"按权重"}};
std::shared_ptr<ImageSource> embedded_logo() {
    const auto module=GetModuleHandleW(nullptr);
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(IDR_FATFISHBOX_LOGO),RT_RCDATA);
    if (!resource) return {};
    const auto loaded=LoadResource(module,resource);
    const auto* bytes=static_cast<const std::uint8_t*>(loaded?LockResource(loaded):nullptr);
    if (!bytes) return {};
    return ImageSource::decode({bytes,SizeofResource(module,resource)});
}
std::wstring title(const Json& object, const char* member) {
    return widen(object.value(member, ""));
}
std::wstring status(const Json& row) {
    static const std::map<std::string,std::wstring> labels{
        {"starting",L"启动中"},{"queued",L"排队"},{"running",L"运行中"},{"waiting_input",L"等待输入"},
        {"succeeded",L"完成"},{"cancelled",L"取消"},{"failed",L"失败"},{"unknown",L"待同步"},{"interrupted",L"中断"}};
    auto found = labels.find(row.value("status",""));
    return found == labels.end() ? L"—" : found->second;
}
ColorToken status_color(const Json& row) {
    const auto state=row.value("status","");
    if (state=="succeeded") return ColorToken::status_success;
    if (state=="failed" || state=="interrupted") return ColorToken::status_error;
    if (state=="waiting_input" || state=="unknown") return ColorToken::status_warning;
    if (state=="running") return ColorToken::accent_primary;
    return ColorToken::text_secondary;
}
std::wstring local_stamp(std::int64_t milliseconds) {
    if (milliseconds <= 0) return L"—";
    const auto seconds=static_cast<std::time_t>(milliseconds/1000);
    std::tm local{};
    wchar_t result[18]{};
    if (localtime_s(&local,&seconds)!=0 || !std::wcsftime(result,std::size(result),L"%m-%d %H:%M",&local)) return L"—";
    return result;
}
std::shared_ptr<const ThemeResources> desktop_theme() {
    auto theme=std::make_shared<ThemeResources>();
    theme->set_accent(Theme::dark,0xffaba9fc);
    for (const auto& [token,color]:std::initializer_list<std::pair<ColorToken,ArgbColor>>{
        {ColorToken::app_background,0xff13151d},{ColorToken::app_surface,0xff191c27},
        {ColorToken::app_card,0xff1b1e29},{ColorToken::app_edge,0xff2d3141},
        {ColorToken::surface_card,0xff1b1e29},{ColorToken::surface_card_edge,0xff2d3141},
        {ColorToken::chrome_caption_background,0xff171923},{ColorToken::control_fill_default,0xff262a39},
        {ColorToken::control_fill_hover,0xff34394b},{ColorToken::text_secondary,0xffb9bfd1},
        {ColorToken::text_tertiary,0xff929ab2}})
        theme->set_color(Theme::dark,token,color);
    return theme;
}

RECT startup_bounds() {
    POINT point{};
    GetCursorPos(&point);
    const auto monitor=MonitorFromPoint(point,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor,&info)) return {CW_USEDEFAULT,CW_USEDEFAULT,1160,760};
    auto work=info.rcWork;
    UINT dpi_x=96,dpi_y=96;
    if (FAILED(GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&dpi_x,&dpi_y))) dpi_x=dpi_y=96;
    const auto width=work.right-work.left, height=work.bottom-work.top;
    const auto fit=[](LONG available,int desired,int minimum,UINT dpi,int percent) {
        return std::min<LONG>(available,std::max<LONG>(MulDiv(minimum,dpi,96),
            std::min<LONG>(MulDiv(desired,dpi,96),MulDiv(available,percent,100))));
    };
    const auto w=fit(width,1240,minimum_width,dpi_x,88), h=fit(height,850,minimum_height,dpi_y,91);
    work.left+=(width-w)/2;work.top+=(height-h)/2;
    work.right=work.left+w;work.bottom=work.top+h;
    return work;
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
        model_.theme_resources = desktop_theme();
        logo_ = embedded_logo();
        renderer_.initialize();
        renderer_.set_dpi(GetDpiForWindow(window));
        try { http_=std::make_unique<fatfish::McpHttpServer>(cluster_); }
        catch (const std::exception& error) { http_error_=widen(error.what()); }
        redraw();
        reload();
    }
    void size(int width, int height) {
        if (!width || !height) return;
        renderer_.resize(static_cast<UINT>(width), static_cast<UINT>(height));
        redraw();
    }
    void dpi(UINT value) { renderer_.set_dpi(value); redraw(); }
    void paint() { renderer_.paint(window, model_, layout_, Theme::dark); }
    void activate(bool active) {
        model_.active = active;
        if (!active) {
            ComboBox::dismiss(model_);
            refresh_combo_layout();
            TextBox::cancel_editing(model_,model_.focused);
            if (GetCapture()==window) ReleaseCapture();
        }
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
        if (drag_list_) {
            const auto id=control_id(viewport(*drag_list_).key);
            if (auto* bar=widget(model_,id); bar && scroll_bar_.pointer_move(*bar,control_bounds(id),dip_point(x,y))) {
                move_list(*drag_list_,ScrollBar::value(*bar));
            }
            return;
        }
        if (const auto* field=widget(model_,model_.pressed); field && field->kind==WidgetKind::textbox &&
            GetCapture()==window) {
            const auto point=dip_point(x,y);
            if (TextBox::pointer(model_,layout_,model_.pressed,point.x,point.y,true))
                InvalidateRect(window,nullptr,FALSE);
            return;
        }
        if (model_.hovered != hit(x,y)) {
            model_.hovered = hit(x,y);
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
        TrackMouseEvent(&track);
    }
    void leave() { model_.hovered = none; InvalidateRect(window, nullptr, FALSE); }
    void down(int x, int y,bool double_click=false) {
        auto which = hit(x,y);
        if (!model_.ui.choice.empty() && which < choice_base) {
            ComboBox::dismiss(model_);
            refresh_combo_layout();
            return;
        }
        auto* w = widget(model_,which);
        if (which < choice_base && !interactive(w)) return;
        SetFocus(window);
        model_.focused = which;
        model_.pressed = which;
        model_.focus_visible = true;
        if (w && ScrollBar::matches(*w)) {
            if (const auto list=list_for(w->key); list &&
                scroll_bar_.pointer_down(*w,control_bounds(which),dip_point(x,y))) {
                drag_list_=list;
                SetCapture(window);
                move_list(*list,ScrollBar::value(*w));
            }
            return;
        }
        if (w && w->kind == WidgetKind::textbox) {
            TextEditor::focus(model_.ui,w->value,which);
            const auto point=dip_point(x,y);
            if (double_click) TextBox::select_word(model_,layout_,which,point.x,point.y);
            else TextBox::pointer(model_,layout_,which,point.x,point.y,GetKeyState(VK_SHIFT)<0);
        }
        SetCapture(window);
        InvalidateRect(window,nullptr,FALSE);
    }
    void up(int x, int y) {
        if (drag_list_) {
            lost_capture();
            if (GetCapture()==window) ReleaseCapture();
            return;
        }
        const int pressed = model_.pressed;
        model_.pressed = none;
        if (GetCapture() == window) ReleaseCapture();
        if (pressed == hit(x,y)) click(pressed);
        InvalidateRect(window,nullptr,FALSE);
    }
    void lost_capture() {
        drag_list_.reset();
        scroll_bar_.pointer_up();
        model_.pressed=none;
        InvalidateRect(window,nullptr,FALSE);
    }
    void wheel(int x,int y,int delta) {
        const auto point=dip_point(x,y);
        if (!model_.ui.choice.empty()) {
            if (!WidgetLayout::contains(layout_.choices,point.x,point.y)) return;
            const float maximum=layout_.choices_extent-(layout_.choices.bottom-layout_.choices.top);
            model_.ui.choice_scroll=std::clamp(model_.ui.choice_scroll-36.f*delta/WHEEL_DELTA,0.f,std::max(0.f,maximum));
            refresh_combo_layout();
        } else {
            for (const auto list:{List::running,List::pending,List::pools}) {
                const auto& view=viewport(list);
                if (WidgetLayout::contains(view.bounds,point.x,point.y)) {
                    UINT lines=3;
                    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
                    const double step=lines==WHEEL_PAGESCROLL?std::max(1.f,(view.bounds.bottom-view.bounds.top)/row_height-2):lines;
                    move_list(list,view.offset-step*delta/WHEEL_DELTA);
                    break;
                }
            }
            if (TextBox::wheel(model_,layout_,point.x,point.y,-0.4f*delta))
                InvalidateRect(window,nullptr,FALSE);
        }
        hover(x,y);
    }
    bool key(WPARAM pressed) {
        if (pressed == VK_TAB) {
            if (ComboBox::dismiss(model_)) refresh_combo_layout();
            std::vector<int> controls;
            for (std::size_t i = 0; i < model_.ui.widgets.size(); ++i) {
                const auto& w = model_.ui.widgets[i];
                if (interactive(&w))
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
        const bool alternate=GetKeyState(VK_MENU)<0;
        const std::map<WPARAM,std::string_view> combo_keys{{VK_UP,alternate?"Alt+Up":"Up"},
            {VK_DOWN,alternate?"Alt+Down":"Down"},{VK_HOME,"Home"},{VK_END,"End"},{VK_ESCAPE,"Escape"},
            {VK_RETURN,"Enter"},{VK_SPACE,"Space"}};
        if (auto key=combo_keys.find(pressed); key!=combo_keys.end()) {
            ComboBoxChange change;
            if (ComboBox::handle_key(model_,key->second,&change)) {
                combo_changed(change);
                return true;
            }
        }
        auto* box = widget(model_,model_.focused);
        if (!box || !box->enabled) return false;
        if (ScrollBar::matches(*box)) {
            const std::map<WPARAM,std::string_view> keys{{VK_UP,"Up"},{VK_DOWN,"Down"},
                {VK_HOME,"Home"},{VK_END,"End"},{VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"}};
            if (const auto key=keys.find(pressed); key!=keys.end() && ScrollBar::key(*box,key->second)) {
                if (const auto list=list_for(box->key)) move_list(*list,ScrollBar::value(*box));
                return true;
            }
        }
        if (box->kind == WidgetKind::button && (pressed == VK_RETURN || pressed == VK_SPACE)) {
            click(model_.focused);
            return true;
        }
        if (box->kind != WidgetKind::textbox) return false;
        const bool control=GetKeyState(VK_CONTROL)<0, extend=GetKeyState(VK_SHIFT)<0;
        if (control && pressed=='V') {
            if (box->read_only) return true;
            if (OpenClipboard(window)) {
                if (HANDLE item=GetClipboardData(CF_UNICODETEXT)) {
                    const auto count=GlobalSize(item)/sizeof(wchar_t);
                    if (const auto* text=static_cast<const wchar_t*>(GlobalLock(item))) {
                        const auto end=std::find(text,text+count,L'\0');
                        if (end!=text+count) insert({text,static_cast<std::size_t>(end-text)});
                        GlobalUnlock(item);
                    }
                }
                CloseClipboard();
            }
            return true;
        }
        if (control && (pressed=='C' || pressed=='X')) {
            const auto text=TextBox::selected_text(model_,model_.focused);
            if (!text.empty() && copy_text(text) && pressed=='X' && !box->read_only) {
                TextEditor::key(model_.ui,box->value,model_.focused,"delete");
                text_changed(*box);
            }
            return true;
        }
        if (control && pressed=='A') {
            TextBox::select_all(model_,model_.focused);
            InvalidateRect(window,nullptr,FALSE);
            return true;
        }
        const std::map<WPARAM,std::string_view> navigation{{VK_HOME,control?"Ctrl+Home":"Home"},
            {VK_END,control?"Ctrl+End":"End"},{VK_UP,"Up"},{VK_DOWN,"Down"},{VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"}};
        if (const auto found=navigation.find(pressed); found!=navigation.end() &&
            TextBox::key(model_,layout_,found->second,extend)) {
            InvalidateRect(window,nullptr,FALSE);
            return true;
        }
        const std::map<WPARAM,std::string_view> editing{{VK_LEFT,control?"word-left":"left"},
            {VK_RIGHT,control?"word-right":"right"},{VK_DELETE,control?"word-delete":"delete"},
            {VK_BACK,control?"word-backspace":"backspace"}};
        const auto found=editing.find(pressed);
        if (found==editing.end()) return false;
        if (!box->read_only || pressed==VK_LEFT || pressed==VK_RIGHT) {
            TextEditor::key(model_.ui,box->value,model_.focused,found->second,extend);
            text_changed(*box);
        }
        return true;
    }
    void type(wchar_t c) {
        if (c>=0xd800 && c<=0xdbff) { surrogate_=c;return; }
        const auto high=std::exchange(surrogate_,wchar_t{});
        if (c>=0xdc00 && c<=0xdfff) {
            if (high) {
                const wchar_t pair[]{high,c};
                insert({pair,std::size(pair)});
            }
            return;
        }
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
            try {
                if (result_ && callback_) callback_(std::move(*result_));
            } catch (const std::exception& error) {
                notice_=widen(error.what());
            }
        } else {
            notice_ = error_;
        }
        redraw();
    }
private:
    Cluster cluster_;
    std::unique_ptr<fatfish::McpHttpServer> http_;
    std::wstring http_error_;
    SurfaceRenderer renderer_;
    WidgetModel model_;
    WidgetLayout layout_{1,1};
    TitleBar title_bar_;
    NavigationView navigation_;
    std::shared_ptr<ImageSource> logo_;
    std::map<std::string,std::wstring> fields_;
    Json running_=Json::array(), pending_=Json::array(), settings_=Json::object(), overview_=Json::object(), editing_, selected_;
    std::vector<std::pair<bool,Json>> pools_;
    std::vector<Json> visible_sessions_;
    std::jthread worker_;
    std::optional<Json> result_;
    std::function<void(Json)> callback_;
    std::wstring notice_, error_, output_;
    wchar_t surrogate_{};
    bool result_loaded_=false;
    std::string page_ = "sessions";
    std::array<ListViewport,3> viewports_{{{"scroll.running"},{"scroll.pending"},{"scroll.pools"}}};
    ScrollBar scroll_bar_;
    std::optional<List> drag_list_;
    bool busy_ = false, enabled_ = false, is_pool_ = false;
    bool navigation_open_ = true;
    Json presets_ = Json::object();

    static bool interactive(const Widget* w) {
        return w && w->enabled && (w->kind==WidgetKind::textbox || w->kind==WidgetKind::button ||
            w->kind==WidgetKind::select || ScrollBar::matches(*w));
    }
    ListViewport& viewport(List list) { return viewports_.at(static_cast<std::size_t>(list)); }
    const ListViewport& viewport(List list) const { return viewports_.at(static_cast<std::size_t>(list)); }
    std::optional<List> list_for(std::string_view key) const {
        for (const auto list:{List::running,List::pending,List::pools})
            if (viewport(list).key==key) return list;
        return {};
    }
    int control_id(std::string_view key) const {
        if (key.empty()) return none;
        const auto it=std::ranges::find(model_.ui.widgets,key,&Widget::key);
        return it==model_.ui.widgets.end()?none:widget_base+static_cast<int>(it-model_.ui.widgets.begin());
    }
    Rect control_bounds(int id) const {
        const auto it=std::ranges::find(layout_.widgets,id,&WidgetPlacement::id);
        return it==layout_.widgets.end()?Rect{}:it->bounds;
    }
    Point dip_point(int x,int y) const {
        const DpiScale scale(GetDpiForWindow(window));
        return {float(scale.dips(x)),float(scale.dips(y))};
    }
    void move_list(List list,double value) {
        auto& view=viewport(list);
        const auto offset=std::clamp(value,0.0,view.maximum);
        if (view.offset==offset) {
            InvalidateRect(window,nullptr,FALSE);
            return;
        }
        view.offset=offset;
        redraw();
    }
    void refresh_combo_layout() {
        rebuild_choice_layout(layout_,model_);
        InvalidateRect(window,nullptr,FALSE);
    }
    void combo_changed(const ComboBoxChange& change) {
        if (change.selection_changed) fields_[change.key]=widen(change.new_value);
        refresh_combo_layout();
        ComboBox::reveal_candidate(model_,layout_);
        refresh_combo_layout();
    }

    void insert(std::wstring_view text) {
        auto* box = widget(model_,model_.focused);
        if (!box || box->kind!=WidgetKind::textbox || !box->enabled || box->read_only) return;
        if (TextEditor::insert(model_.ui,box->value,model_.focused,text))
            text_changed(*box);
    }
    void text_changed(const Widget& box) {
        if (!box.read_only && !box.binding.empty()) fields_[box.binding]=box.value;
        TextBox::reveal(model_,layout_);
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
        for (auto key : {"configPath","authPath","permission","preset","balance"})
            fields_[key] = title(settings_,key);
        refresh_presets();
    }
    void refresh_presets() {
        presets_=Json::object();
        call("cluster_get_presets",paths(),[this](Json data) {
            if (!data.value("connected",false)) notice_=L"设置 DSH 连接后可读取完整预设";
            presets_=std::move(data);
        });
    }
    bool copy_text(const std::wstring& value) {
        const auto bytes=(value.size()+1)*sizeof(wchar_t);
        const auto buffer=GlobalAlloc(GMEM_MOVEABLE,bytes);
        if (!buffer) return false;
        if (auto* text=GlobalLock(buffer)) {
            memcpy(text,value.c_str(),bytes);
            GlobalUnlock(buffer);
        } else {
            GlobalFree(buffer);
            return false;
        }
        if (!OpenClipboard(window)) {
            GlobalFree(buffer);
            return false;
        }
        const bool copied=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,buffer);
        CloseClipboard();
        if (!copied) GlobalFree(buffer);
        return copied;
    }
    void copy_http_token() {
        if (!http_) return;
        notice_=copy_text(widen(http_->token()))?L"已复制 MCP 令牌":L"剪贴板暂不可用";
        redraw();
    }
    void local_snapshot() {
        try {
            auto snapshot=cluster_.desktop_snapshot();
            overview_=std::move(snapshot.at("overview"));
            settings_=std::move(snapshot.at("settings"));
            running_=std::move(snapshot.at("running"));
            pending_=std::move(snapshot.at("pending"));
            update_pools();
        } catch (const std::exception& e) { notice_ = widen(e.what()); }
    }
    void update_pools() {
        pools_.clear();
        for (const auto& account:settings_.at("accounts")) pools_.emplace_back(false,account);
        for (const auto& pool:settings_.at("pools")) pools_.emplace_back(true,pool);
    }
    void reload() { local_snapshot(); redraw(); }
    void nav(std::string to) {
        ComboBox::dismiss(model_);
        TextBox::cancel_editing(model_,model_.focused);
        model_.ui.editor_focus=none;
        surrogate_=0;
        page_ = std::move(to);
        model_.focused = none;
        model_.hovered = none;
        model_.pressed = none;
        notice_.clear();
        if (page_ == "connection") load_connection();
        redraw();
    }
    void clear_result() {
        output_.clear();
        result_loaded_=false;
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
    void label(std::wstring text,float x,float y,float w,float size=14,std::string surface="root",
               ColorToken color=ColorToken::text_primary,bool strong=false) {
        if (w<=0 || text.empty()) return;
        const auto h=std::max(23.f,size*1.45f);
        auto& item=add("",WidgetKind::label,L"",x,y,w,h,std::move(surface));
        TextBlock({.text=std::move(text),.font_role=strong||size>=20?FontRole::body_strong:FontRole::body,
            .font_size=size,.wrap=false,.max_lines=1,.width=w,.height=h,.foreground=color}).apply(item);
    }
    void button(std::string key,std::wstring name,float x,float y,float w,bool primary=false,bool allowed=true,std::string surface="root") {
        auto& item = add(std::move(key),WidgetKind::button,name,x,y,w,32,std::move(surface));
        Button(ButtonOptions{.text=std::move(name),.primary=primary,.enabled=allowed && !busy_,.width=w}).apply(item);
    }
    void field(std::string key,std::wstring name,float x,float y,float w,const std::string& surface="root") {
        label(std::move(name),x,y,w,13,surface,ColorToken::text_secondary);
        auto& item = add("input."+key,WidgetKind::textbox,L"",x,y+24,w,32,surface);
        item.binding = key;
        TextBox(TextBoxOptions{.text=fields_[key],.enabled=!busy_}).apply(item);
    }
    void combo(std::string key,std::wstring name,float x,float y,float w,std::vector<Choice> options,const std::string& surface="root") {
        label(name,x,y,w,13,surface,ColorToken::text_secondary);
        const auto value=narrow(fields_[key]);
        if (std::ranges::find(options,value,&Choice::key)==options.end()) options.push_back({value,fields_[key]});
        auto& item=add(key,WidgetKind::select,L"",x,y+24,w,32,surface);
        ComboBox({std::move(name),std::move(options),fields_[key],!busy_,w}).apply(item);
    }
    std::vector<Choice> preset_options(const char* key,const wchar_t* placeholder) const {
        std::vector<Choice> options{{"",placeholder}};
        for (const auto& row:presets_.value(key,Json::array()))
            options.push_back({row.at("id").get<std::string>(),title(row,"label")});
        return options;
    }
    int list_start(List list,const std::string& surface,Rect box,int total,int capacity,std::wstring name) {
        auto& view=viewport(list);
        view.bounds={box.left,box.top+title_bar_.height(),box.right,box.bottom+title_bar_.height()};
        view.maximum=std::max(0,total-capacity);
        view.offset=std::clamp(view.offset,0.0,view.maximum);
        if (view.maximum>0 && box.bottom-box.top>=16) {
            auto& bar=ScrollBar({.label=std::move(name),.maximum=view.maximum,.value=view.offset,
                .viewport=static_cast<double>(capacity),.length=box.bottom-box.top}).append(model_,surface,view.key);
            bar.area=WidgetArea::absolute;
            bar.bounds={box.right-18,view.bounds.top,box.right-2,view.bounds.bottom};
        }
        return static_cast<int>(std::lround(view.offset));
    }
    void table_row(std::string key,std::string action,float x,float y,float w,const std::string& surface) {
        auto& row=add(std::move(key),WidgetKind::button,L"",x,y,w,row_height-1,surface);
        row.action=std::move(action);
        row.enabled=!busy_;
        row.visual=WidgetVisual::list_view_item;
    }
    void session_section(List list,const Json& rows,float x,float y,float right,float bottom) {
        const bool running=list==List::running;
        const std::string surface=running?"running":"pending";
        const float left=x+16, table_right=right-32;
        const float width=table_right-left;
        const bool time_column=width>=650;
        const float model=left+width*(time_column?.57f:.67f);
        const float state=table_right-(time_column?175.f:78.f);
        const float time=table_right-92;
        const int total=int(rows.size());
        const int capacity=std::max(1,int((bottom-y-70)/row_height));
        panel(surface,{x,y,right,bottom});
        const int start=list_start(list,surface,{x+8,y+61,right-7,bottom-8},total,capacity,
            running?L"滚动进行中的会话":L"滚动已完成待处理的会话");
        const auto heading=running?L"进行中":L"已完成 · 未处理";
        label(heading,left,y+11,width-125,17,surface,ColorToken::text_primary,true);
        const int end=std::min(start+capacity,total);
        label(total>capacity?std::to_wstring(start+1)+L"–"+std::to_wstring(end)+L" / "+std::to_wstring(total):
            std::to_wstring(total)+L" 个会话",table_right-113,y+15,113,12,surface,ColorToken::text_secondary);
        constexpr auto quiet=ColorToken::text_tertiary;
        label(L"会话",left,y+42,model-left-12,12,surface,quiet);
        label(L"模型",model,y+42,state-model-8,12,surface,quiet);
        label(L"状态",state,y+42,time_column?75.f:78.f,12,surface,quiet);
        if (time_column) label(running?L"最近更新":L"完成时间",time,y+42,92,12,surface,quiet);
        if (!total) label(running?L"当前没有进行中的会话":L"当前没有已完成待处理的会话",
            left,y+73,width,14,surface,ColorToken::text_secondary);
        for (int i=start;i<end;++i) {
            const auto& row=rows.at(i);
            const float line=y+61+float(i-start)*row_height;
            const auto action="row."+std::to_string(visible_sessions_.size());
            visible_sessions_.push_back(row);
            table_row("session."+row.at("id").get<std::string>(),action,x+8,line,table_right-x,surface);
            label(title(row,"title"),left,line+7,model-left-12,14,surface);
            auto model_name=title(row,"model");
            if (model_name.empty()) model_name=L"—";
            label(std::move(model_name),model,line+7,state-model-8,13,surface,ColorToken::text_secondary);
            label(L"● "+status(row),state,line+7,time_column?76.f:78.f,12,surface,status_color(row));
            if (time_column) label(local_stamp(row.value(running?"updated":"completed",std::int64_t(0))),
                time,line+7,92,12,surface,ColorToken::text_secondary);
        }
    }
    void session_page(float x,float right,float h) {
        button("sync",L"同步会话",right-104,20,104);
        const auto usage=overview_.value("usage",Json::object());
        const auto total=usage.value("total",Json::object());
        const auto input=total.value("uncachedInputTokens",0.0), read=total.value("cacheReadTokens",0.0);
        const auto output=total.value("outputTokens",0.0), write=total.value("cacheWriteTokens",0.0);
        const auto billed=input+read+write;
        const auto hourly=usage.value("hourlyTokens",Json(nullptr));
        const std::array<std::pair<const wchar_t*,std::wstring>,5> values{{
            {L"今日平均每小时",hourly.is_number()?compact_tokens(hourly.get<double>())+L" Token/h":L"—"},
            {L"未缓存输入",compact_tokens(input)},{L"输出",compact_tokens(output)},
            {L"缓存读取",compact_tokens(read)},{L"缓存命中",billed>0?fixed3(read*100/billed)+L"%":L"—"}
        }};
        panel("usage",{x,64,right,127});
        const float usable=right-x-32, first=usable*.29f, cell=(usable-first)/4;
        for (std::size_t i=0;i<values.size();++i) {
            const float left=x+16+(i?first+float(i-1)*cell:0);
            const float width=(i?cell:first)-8;
            label(values[i].first,left,73,width,12,"usage",ColorToken::text_secondary);
            label(values[i].second,left,92,width,15,"usage",i?ColorToken::text_primary:ColorToken::accent_primary,true);
        }
        constexpr float start=140, gap=12;
        const float tail=h-30, available=tail-start-gap;
        const float ideal=71+row_height*static_cast<float>(std::clamp(running_.size(),std::size_t{1},std::size_t{5}));
        const float active_height=std::min(ideal,std::floor(available*.47f));
        visible_sessions_.clear();
        session_section(List::running,running_,x,start,right,start+active_height);
        session_section(List::pending,pending_,x,start+active_height+gap,right,tail);
        if (notice_.empty()) {
            auto summary=overview_.value("codexSources",0)>0?
                L"Codex 登记快照 · "+std::to_wstring(overview_.value("codexSessions",0))+L" 个会话":L"";
            if (overview_.value("unreadableCodexSources",0)>0) summary+=summary.empty()?L"部分 Codex 登记不可读":L"  ·  部分登记不可读";
            if (usage.value("missing",0)>0) summary+=summary.empty()?L"Token 统计不完整":L"  ·  Token 统计不完整";
            label(std::move(summary),x,h-24,right-x,12,"root",ColorToken::text_secondary);
        }
    }
    void connection_page(float x,float right) {
        const float width=right-x;
        const bool side=width>=860;
        const float config_right=side?x+width*.62f:right;
        const float left=x+16, form=config_right-left-16;
        const float half=(form-14)/2;
        panel("dsh",{x,64,config_right,346});
        label(L"DeepSeek Harness",left,76,form,17,"dsh",ColorToken::text_primary,true);
        field("configPath",L"DSH 配置文件",left,104,form,"dsh");
        field("authPath",L"本机授权文件（可从配置自动识别）",left,164,form,"dsh");
        combo("permission",L"权限预设",left,224,half,preset_options("permissions",L"请选择权限预设"),"dsh");
        combo("preset",L"Agent 预设",left+half+14,224,half,preset_options("agents",L"DSH 默认"),"dsh");
        combo("balance",L"分配策略",left,282,half,balance_options,"dsh");
        button("refresh_presets",L"刷新预设",config_right-248,306,96,false,true,"dsh");
        button("save_connection",L"保存连接设置",config_right-142,306,126,true,true,"dsh");
        button("import",L"从 DSH 导入模型",right-154,20,154,true);

        const float mcp_x=side?config_right+12:x;
        const float mcp_y=side?64.f:358.f;
        panel("mcp",{mcp_x,mcp_y,right,mcp_y+(side?166.f:106.f)});
        label(L"MCP 连接",mcp_x+16,mcp_y+12,right-mcp_x-32,17,"mcp",ColorToken::text_primary,true);
        if (http_) {
            label(L"HTTP 监听中",mcp_x+16,mcp_y+42,right-mcp_x-132,12,"mcp",ColorToken::status_success);
            label(widen(http_->url()),mcp_x+16,mcp_y+62,right-mcp_x-132,13,"mcp");
            button("copy_token",L"复制令牌",right-108,mcp_y+42,92,false,true,"mcp");
        } else label(http_error_,mcp_x+16,mcp_y+62,right-mcp_x-32,13,"mcp",ColorToken::status_error);
        if (side) label(L"stdio  ·  FatFishBox.exe --mcp",mcp_x+16,mcp_y+118,right-mcp_x-32,13,"mcp",ColorToken::text_secondary);
        else label(L"stdio  ·  FatFishBox.exe --mcp",mcp_x+16,mcp_y+83,right-mcp_x-32,12,"mcp",ColorToken::text_secondary);
    }
    void pools_page(float x,float right,float h) {
        button("import",L"从 DSH 导入模型",right-154,20,154,true);
        const float left=x+16, width=right-x;
        const auto strategy=std::ranges::find(balance_options,settings_.value("balance","least_loaded"),&Choice::key);
        const int accounts=int(settings_.value("accounts",Json::object()).size());
        const int models=int(settings_.value("pools",Json::object()).size());
        auto description=std::to_wstring(accounts)+L" 个账号  ·  "+std::to_wstring(models)+L" 个模型池";
        if (strategy!=balance_options.end()) description+=L"  ·  "+strategy->label;
        label(std::move(description),x,56,width,13,"root",ColorToken::text_secondary);
        const float top=85, bottom=h-30;
        panel("poolrows",{x,top,right,bottom});
        const int total=int(pools_.size()), capacity=std::max(1,int((bottom-top-50)/row_height));
        const int start=list_start(List::pools,"poolrows",{x+8,top+41,right-7,bottom-8},total,capacity,L"滚动账号与模型池");
        const float model=x+width*.14f, state=x+width*.63f, limit=x+width*.77f;
        label(L"类型",left,top+12,model-left-8,12,"poolrows",ColorToken::text_tertiary);
        label(L"账号或模型",model,top+12,state-model-10,12,"poolrows",ColorToken::text_tertiary);
        label(L"状态",state,top+12,limit-state-6,12,"poolrows",ColorToken::text_tertiary);
        label(L"并发 · 思考强度",limit,top+12,right-limit-32,12,"poolrows",ColorToken::text_tertiary);
        for (int i=start;i<std::min(start+capacity,total);++i) {
            const auto& [pool,row]=pools_[i];
            const float y=top+41+float(i-start)*row_height;
            table_row("pool."+row.at("id").get<std::string>(),"pool."+std::to_string(i),x+8,y,width-40,"poolrows");
            label(pool?L"模型池":L"账号",left,y+7,model-left-8,12,"poolrows",ColorToken::text_secondary);
            auto name=title(row,"name");
            if (pool) name+=L"  ·  "+title(row,"provider");
            label(std::move(name),model,y+7,state-model-10,14,"poolrows");
            const bool enabled=row.value("enabled",false);
            label(enabled?L"● 启用":L"○ 关闭",state,y+7,limit-state-6,12,"poolrows",
                enabled?ColorToken::status_success:ColorToken::text_secondary);
            auto concurrency=std::to_wstring(row.value("concurrency",1));
            if (pool) concurrency+=row.value("effort","").empty()?L"  ·  DSH 默认":L"  ·  "+title(row,"effort");
            label(std::move(concurrency),limit,y+7,right-limit-32,12,"poolrows",ColorToken::text_secondary);
        }
        if (!total) label(L"从 DSH 导入模型后可在这里编辑调度配置。",left,top+55,width-32,14,"poolrows",ColorToken::text_secondary);
    }
    void redraw() {
        if (!window) return;
        RECT rect{};
        GetClientRect(window,&rect);
        const DpiScale scale(GetDpiForWindow(window));
        const float w=float(scale.dips(rect.right)), client_height=float(scale.dips(rect.bottom));
        if (w < 100 || client_height < 100) return;
        const bool maximized = IsZoomed(window) != FALSE;
        const auto* focus=widget(model_,model_.focused);
        const auto focus_key=focus?focus->key:std::string{};
        const auto* hover=widget(model_,model_.hovered);
        const auto hover_key=hover?hover->key:std::string{};
        const auto* pressed=widget(model_,model_.pressed);
        const auto pressed_key=pressed?pressed->key:std::string{};
        title_bar_.configure({
            .title=L"肥鱼重工", .minimize_label=L"最小化",
            .maximize_label=maximized?L"还原":L"最大化", .close_label=L"关闭",
            .maximized=maximized, .pane_toggle=true, .pane_toggle_label=L"切换导航栏",
        });
        const float h = client_height-title_bar_.height();
        layout_=WidgetLayout(w,client_height);
        model_.ui.widgets.clear();
        model_.ui.surfaces.clear();
        for (auto& view:viewports_) view.bounds={};
        panel("root",{0,0,w,client_height},SurfaceVisual::transparent);
        title_bar_.append(model_,w);
        if (logo_ && logo_->status()==ImageStatus::ready) {
            const auto icon=std::ranges::find(model_.ui.widgets,"titlebar.icon",&Widget::key);
            if (icon!=model_.ui.widgets.end()) {
                icon->bounds={icon->bounds.left-2,icon->bounds.top-2,icon->bounds.right+2,icon->bounds.bottom+2};
                Image({.source=logo_,.width=20,.height=20,.description=L"肥鱼重工"}).apply(*icon);
            }
        }
        const auto selected_page=page_=="pools" || page_=="edit" ? "pools" :
            page_=="connection" ? "connection" : "sessions";
        NavigationViewOptions navigation;
        navigation.id="sidebar";
        navigation.pane_expanded=navigation_open_ && w>=minimum_width;
        navigation.selected_id=selected_page;
        navigation.items={{"sessions",L"会话与交接",L"\uE8BD",!busy_,{}},
                          {"pools",L"账号与模型池",L"\uE716",!busy_,{}}};
        navigation.settings={"connection",L"连接与 MCP",L"\uE713",!busy_,{}};
        navigation.expanded_width=w<900?166.f:196.f;
        navigation.pane_toggle_visible=false;
        navigation.search_visible=false;
        navigation_.configure(std::move(navigation));
        navigation_.append(model_,w,client_height,title_bar_.height());
        const float inset=w<900?18.f:26.f;
        const float content=w-navigation_.width()-2*inset;
        const float width=std::min(content,1440.f), x=navigation_.width()+inset+(content-width)/2, right=x+width;
        const std::map<std::string,std::wstring> names{{"sessions",L"会话与交接"},{"pools",L"账号与模型池"},
            {"connection",L"连接与 MCP"},{"detail",L"会话详情"},{"edit",L"调度配置"}};
        label(names.at(page_),x,19,width-246,25);
        if (!notice_.empty()) label(notice_,x,h-24,width,12,"root",ColorToken::text_secondary);
        if (page_=="sessions") {
            session_page(x,right,h);
        } else if (page_=="connection") {
            connection_page(x,right);
        } else if (page_=="pools") {
            pools_page(x,right,h);
        } else if (page_=="edit" && editing_.is_object()) {
            button("nav.pools",L"返回模型池",right-112,20,112);
            const float left=x+16, form=width-32, third=(form-24)/3;
            panel("limits",{x,64,right,258});
            label(title(editing_,"name"),left,77,form-120,18,"limits",ColorToken::text_primary,true);
            label(title(editing_,is_pool_?"provider":"id"),left,102,form-120,13,"limits",ColorToken::text_secondary);
            button("enabled",enabled_?L"已启用":L"已关闭",right-112,79,96,enabled_,true,"limits");
            field("concurrency",L"总并发",left,132,third,"limits");
            if (is_pool_) {
                field("priority",L"优先级（越小越优先）",left+third+12,132,third,"limits");
                field("weight",L"权重",left+2*(third+12),132,third,"limits");
                std::vector<Choice> efforts{{"",L"DSH 默认"}};
                for (const auto& effort:editing_.at("efforts")) efforts.push_back({effort.get<std::string>(),widen(effort.get<std::string>())});
                combo("effort",L"思考强度",left,194,third,std::move(efforts),"limits");
            }
            panel("roles",{x,270,right,387});
            label(L"角色并发",left,280,form,17,"roles",ColorToken::text_primary,true);
            const float part=(form-36)/4;
            for (std::size_t i=0;i<roles.size();++i)
                field(roles[i],role_names[i],left+float(i)*(part+12),312,part,"roles");
            button("save_pool",L"保存调度配置",right-132,400,132,true);
        } else if (page_=="detail" && selected_.is_object()) {
            button("nav.sessions",L"返回会话",right-110,20,110);
            const float left=x+16, inner=width-32;
            panel("session_detail",{x,64,right,219});
            label(title(selected_,"title"),left,76,inner,19,"session_detail",ColorToken::text_primary,true);
            const auto batch=title(selected_,"batch"), version=title(selected_,"resultVersion");
            label(status(selected_)+(batch.empty()?L"":L"  ·  批次 "+batch)+(version.empty()?L"":L"  ·  结果版本 "+version),
                left,107,inner,13,"session_detail",status_color(selected_));
            label(L"工作区  "+title(selected_,"workspace"),left,132,inner,13,"session_detail",ColorToken::text_secondary);
            const bool linked=selected_.contains("dshId");
            const bool receipt=!selected_.value("resultVersion","").empty();
            const bool legacy=selected_.value("readOnly",false);
            button("refresh_row",L"刷新状态",left,174,92,false,linked,"session_detail");
            button("read_row",L"读取结果",left+102,174,92,false,linked,"session_detail");
            if (legacy) {
                label(L"来自 Codex · 请在原插件中确认或取消",left+208,181,inner-208,12,"session_detail",ColorToken::text_secondary);
            } else {
                button("cancel_row",L"取消",left+204,174,72,false,linked&&!receipt,"session_detail");
                button("mark_done",L"确认处理",left+286,174,104,true,receipt&&!selected_.value("processed",false),"session_detail");
            }
            panel("result",{x,231,right,h-30});
            label(L"结果",left,242,inner,17,"result",ColorToken::text_primary,true);
            auto text=result_loaded_?output_:title(selected_,"error");
            if (!text.empty()) {
                button("copy_result",L"复制结果",right-104,237,88,false,true,"result");
                auto& box=add("output",WidgetKind::textbox,L"",left,273,inner,h-318,"result");
                TextBox(TextBoxOptions{.text=text,.read_only=true,.multiline=true,.height=h-318}).apply(box);
            } else label(result_loaded_?L"当前结果没有可显示的文本":L"尚未读取结果",
                left,278,inner,13,"result",ColorToken::text_secondary);
        }
        append_widget_layout(layout_,model_);
        if (model_.focused<choice_base) model_.focused=control_id(focus_key);
        if (model_.hovered<choice_base) model_.hovered=control_id(hover_key);
        if (model_.pressed<choice_base) model_.pressed=control_id(pressed_key);
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
        if (control>=choice_base) {
            ComboBoxChange change;
            if (ComboBox::select(model_,static_cast<std::size_t>(control-choice_base),&change))
                combo_changed(change);
            return;
        }
        auto* w=widget(model_,control);
        if (!interactive(w)) return;
        if (w->kind==WidgetKind::select) {
            if (ComboBox::open(model_,control)) combo_changed({});
            return;
        }
        if (w->kind!=WidgetKind::button) return;
        if (w->key=="window.minimize") { ShowWindow(window,SW_MINIMIZE); return; }
        if (w->key=="window.maximize") { ShowWindow(window,IsZoomed(window)?SW_RESTORE:SW_MAXIMIZE); return; }
        if (w->key=="window.close") { PostMessageW(window,WM_CLOSE,0,0); return; }
        if (w->key=="titlebar.pane-toggle") { navigation_open_=!navigation_open_; redraw(); return; }
        if (busy_) return;
        if (auto page=navigation_.item_id(w->key); !page.empty()) { nav(std::move(page)); return; }
        const auto name=w->action;
        try {
            if (name.starts_with("nav.")) {nav(name.substr(4));return;}
            if (name=="refresh_presets") {refresh_presets();return;}
            if (name=="copy_token") {copy_http_token();return;}
            if (name=="copy_result") {
                notice_=copy_text(result_loaded_?output_:title(selected_,"error"))?L"已复制结果":L"剪贴板暂不可用";
                redraw();return;
            }
            if (name=="enabled") {enabled_=!enabled_;redraw();return;}
            if (name.starts_with("row.")) {selected_=visible_sessions_.at(std::stoul(name.substr(4)));clear_result();nav("detail");return;}
            if (name.starts_with("pool.")) {
                auto [is_pool,row]=pools_.at(std::stoul(name.substr(5)));
                editing_=row;is_pool_=is_pool;enabled_=row.value("enabled",false);
                fields_["concurrency"]=std::to_wstring(row.value("concurrency",1));
                for (auto role:roles) fields_[role]=row.at("roles").contains(role)?std::to_wstring(row.at("roles").at(role).get<int>()):L"";
                if (is_pool) {
                    for (auto key:{"priority","weight"}) fields_[key]=std::to_wstring(row.value(key,0));
                    fields_["effort"]=title(row,"effort");
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
                    {"preset",narrow(fields_.at("preset"))},{"balance",narrow(fields_.at("balance"))}});
                call("cluster_configure",input,[this](Json value){settings_=std::move(value);local_snapshot();notice_=L"连接设置已保存";});
            } else if (name=="save_pool") {
                Json caps=Json::object();
                for (auto role:roles) if (!fields_.at(role).empty()) caps[role]=number(role);
                Json input{{"revision",settings_.at("revision")},{"id",editing_.at("id")},{"enabled",enabled_},
                    {"concurrency",number("concurrency")},{"roles",caps}};
                if (is_pool_) {
                    input["priority"]=number("priority");input["weight"]=number("weight");
                    input["effort"]=narrow(fields_.at("effort"));
                }
                call(is_pool_?"cluster_update_pool":"cluster_update_account",input,[this](Json value) {
                    settings_=std::move(value);update_pools();nav("pools");notice_=L"调度配置已保存";
                });
            } else if (name.ends_with("_row") || name.starts_with("mark_")) {
                const std::map<std::string,std::string> operations{{"refresh_row","cluster_refresh_session"},{"cancel_row","cluster_cancel_session"},
                    {"read_row","cluster_read_result"},{"mark_done","cluster_mark_receipt"}};
                Json input{{"sessionId",selected_.at("id")}};
                if (name.starts_with("mark_")) input.update(Json{{"resultVersion",selected_.at("resultVersion")},{"processed",name=="mark_done"}});
                call(operations.at(name),input,[this,name](Json value) {
                    if (name=="read_row") {
                        clear_result();
                        if (value.at("resultVersion")!=selected_.at("resultVersion")) {
                            notice_=L"结果版本已变化，请先刷新状态";
                            return;
                        }
                        for (const auto& message:value.at("messages")) {
                            if (!output_.empty()) output_+=L"\r\n\r\n";
                            output_+=title(message,"text");
                        }
                        result_loaded_=true;
                    } else {
                        if (value.at("resultVersion")!=selected_.at("resultVersion") ||
                            value.at("status")!=selected_.at("status")) clear_result();
                        selected_=std::move(value);local_snapshot();
                    }
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
        info->ptMinTrackSize={MulDiv(minimum_width,scale,96),MulDiv(minimum_height,scale,96)};
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
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd,&point);
        app->wheel(point.x,point.y,GET_WHEEL_DELTA_WPARAM(wp));return 0;
    }
    case WM_LBUTTONDOWN: app->down(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_LBUTTONDBLCLK: app->down(GET_X_LPARAM(lp),GET_Y_LPARAM(lp),true);return 0;
    case WM_LBUTTONUP: app->up(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_CAPTURECHANGED: app->lost_capture();return 0;
    case WM_KEYDOWN: if (app->key(wp)) return 0;break;
    case WM_SYSKEYDOWN: if ((wp==VK_UP || wp==VK_DOWN) && app->key(wp)) return 0;break;
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
        const std::wstring mode=argc==2?argv[1]:L"";
        LocalFree(argv);
        if (mode==L"--mcp") return fatfish::run_mcp_stdio();
        if (mode==L"--mcp-http") return fatfish::run_mcp_http();
    }
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        App app;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style=CS_DBLCLKS;
        wc.hInstance=instance;wc.lpfnWndProc=dispatch_window;wc.lpszClassName=L"FatFishBoxWindow";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_FATFISHBOX),IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));
        wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_FATFISHBOX),IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        if (!RegisterClassExW(&wc)) throw std::runtime_error("窗口类注册失败");
        const auto bounds=startup_bounds();
        const auto hwnd=CreateWindowExW(0,wc.lpszClassName,L"肥鱼重工",WS_OVERLAPPEDWINDOW,
            bounds.left,bounds.top,bounds.left==CW_USEDEFAULT?bounds.right:bounds.right-bounds.left,
            bounds.top==CW_USEDEFAULT?bounds.bottom:bounds.bottom-bounds.top,nullptr,nullptr,instance,&app);
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
