#include "cluster.h"
#include "legacy.h"
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
#include <native_ui/toggle_switch.h>
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
#include <set>
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
constexpr UINT usage_finished = WM_APP + 2;
constexpr UINT sessions_finished = WM_APP + 3;
constexpr UINT_PTR usage_timer = 1;
constexpr UINT_PTR sessions_timer = 2;
constexpr UINT sessions_interval_ms = 5000;
constexpr int minimum_width=740, minimum_height=520;
constexpr float row_height=34;
constexpr float pool_row_height=52;
enum class List { running, pending, enabled_pools, all_pools };
constexpr std::array lists{List::running,List::pending,List::enabled_pools,List::all_pools};
struct ListViewport {
    std::string key;
    Rect bounds{};
    double offset=0, maximum=0;
};
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
        {"cancelling_policy_violation",L"正在停止"},
        {"succeeded",L"完成"},{"cancelled",L"取消"},{"failed",L"失败"},{"unknown",L"待同步"},{"interrupted",L"中断"}};
    auto found = labels.find(row.value("status",""));
    return found == labels.end() ? L"—" : found->second;
}
ColorToken status_color(const Json& row) {
    const auto state=row.value("status","");
    if (row.value("stale",false)) return ColorToken::status_warning;
    if (state=="succeeded") return ColorToken::status_success;
    if (state=="failed" || state=="interrupted") return ColorToken::status_error;
    if (state=="waiting_input" || state=="unknown") return ColorToken::status_warning;
    if (state=="running") return ColorToken::accent_primary;
    return ColorToken::text_secondary;
}
std::wstring local_stamp(std::int64_t milliseconds,const wchar_t* format=L"%m-%d %H:%M") {
    if (milliseconds <= 0) return L"—";
    const auto seconds=static_cast<std::time_t>(milliseconds/1000);
    std::tm local{};
    wchar_t result[18]{};
    if (localtime_s(&local,&seconds)!=0 || !std::wcsftime(result,std::size(result),format,&local)) return L"—";
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
    ~App() {
        usage_worker_.request_stop();
        sessions_worker_.request_stop();
        if (usage_worker_.joinable()) usage_worker_.join();
        if (sessions_worker_.joinable()) sessions_worker_.join();
        if (worker_.joinable()) worker_.join();
    }
    void init() {
        model_.theme = Theme::dark;
        model_.theme_resources = desktop_theme();
        logo_ = embedded_logo();
        renderer_.initialize();
        renderer_.set_dpi(GetDpiForWindow(window));
        try {cluster_.migrate_repair_queue();}
        catch (const std::exception& error) {notice_=widen(error.what());}
        try { http_=std::make_unique<fatfish::McpHttpServer>(cluster_); }
        catch (const std::exception& error) { http_error_=widen(error.what()); }
        redraw();
        sync_usage();
        reload();
        SetTimer(window,usage_timer,60000,nullptr);
        sessions_timer_ready_=SetTimer(window,sessions_timer,sessions_interval_ms,nullptr)!=0;
        refresh_sessions_automatically();
    }
    void size(int width, int height) {
        if (!width || !height) return;
        renderer_.resize(static_cast<UINT>(width), static_cast<UINT>(height));
        redraw();
    }
    void dpi(UINT value) { renderer_.set_dpi(value); redraw(); }
    void paint() { renderer_.paint(window, model_, layout_, Theme::dark); }
    void closing() { commit_concurrency(); }
    void activate(bool active) {
        model_.active = active;
        if (!active) {
            commit_concurrency();
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
        if (which!=model_.focused) {
            commit_concurrency();
            which=hit(x,y);
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
            for (const auto list:lists) {
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
            commit_concurrency();
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
        if (box->visual==WidgetVisual::toggle_switch && pressed==VK_SPACE)
            return ToggleSwitch::key_down(model_,"Space");
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
        if (pressed==VK_RETURN && box->binding.starts_with("schedule.concurrency.")) {
            commit_concurrency();
            return true;
        }
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
    void key_up(WPARAM key) {
        if (key!=VK_SPACE) return;
        const auto* field=widget(model_,model_.focused);
        if (field && field->visual==WidgetVisual::toggle_switch && ToggleSwitch::key_up(model_,"Space"))
            save_schedule(field->action,field->selected);
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
            if (page_=="pools") {
                try { settings_=cluster_.invoke("cluster_get_settings");update_pools(); }
                catch (const std::exception&) {}
            }
        }
        redraw();
    }
    void sync_usage() {
        if (usage_busy_) return;
        if (usage_worker_.joinable()) usage_worker_.join();
        usage_busy_=true;
        usage_worker_=std::jthread([this](std::stop_token stop) {
            try { cluster_.refresh_legacy_usage(stop); }
            catch (const std::exception&) {}
            if (!stop.stop_requested()) PostMessageW(window,usage_finished,0,0);
        });
    }
    void receive_usage() {
        if (usage_worker_.joinable()) usage_worker_.join();
        usage_busy_=false;
        if (page_=="sessions" && GetCapture()!=window && !sessions_worker_.joinable())
            refresh_sessions_automatically();
    }
    void refresh_sessions_automatically(bool manual=false) {
        if (page_!="sessions" && page_!="detail") return;
        if (manual) {
            sync_usage();
            refresh_requested_=sessions_worker_.joinable();
        }
        if (IsIconic(window) || sessions_worker_.joinable()) return;
        if (sessions_snapshot_) apply_session_refresh();
        sessions_generation_=snapshot_generation_;
        sessions_worker_=std::jthread([this](std::stop_token stop) {
            try {
                sessions_snapshot_=cluster_.desktop_snapshot(&desktop_monitor_,stop);
                sessions_error_.clear();
            } catch (const std::exception& error) { sessions_error_=widen(error.what()); }
            if (!stop.stop_requested()) PostMessageW(window,sessions_finished,0,0);
        });
        if (GetCapture()!=window) redraw();
    }
    void receive_sessions() {
        if (sessions_worker_.joinable()) sessions_worker_.join();
        apply_session_refresh();
        if (std::exchange(refresh_requested_,false)) refresh_sessions_automatically();
    }
    void apply_session_refresh() {
        if (sessions_worker_.joinable() || GetCapture()==window || !model_.ui.choice.empty()) return;
        auto snapshot=std::exchange(sessions_snapshot_,std::nullopt);
        if (sessions_generation_!=snapshot_generation_) return;
        if (snapshot) {
            apply_sessions(*snapshot);
            session_sync_at_=fatfish::now();
            session_live_checked_=snapshot->at("liveChecked").get<int>();
            session_live_stale_=snapshot->at("liveStale").get<int>();
        }
        if (page_=="sessions" || page_=="detail") redraw();
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
    Json running_=Json::array(), pending_=Json::array(), settings_=Json::object(), overview_=Json::object(), selected_;
    std::map<std::string,std::vector<std::string>> account_models_;
    std::set<std::string> expanded_accounts_;
    std::vector<Json> visible_sessions_;
    std::jthread worker_;
    std::jthread usage_worker_;
    std::jthread sessions_worker_;
    std::optional<Json> sessions_snapshot_;
    fatfish::DesktopMonitor desktop_monitor_;
    std::wstring sessions_error_;
    std::uint64_t snapshot_generation_=0,sessions_generation_=0;
    std::int64_t session_sync_at_=0;
    int session_live_checked_=0,session_live_stale_=0;
    bool refresh_requested_=false,sessions_timer_ready_=false;
    bool usage_busy_=false;
    std::optional<Json> result_;
    std::function<void(Json)> callback_;
    std::wstring notice_, error_, output_;
    wchar_t surrogate_{};
    bool result_loaded_=false;
    std::string page_ = "sessions";
    std::array<ListViewport,4> viewports_{{{"scroll.running"},{"scroll.pending"},{"scroll.enabled_pools"},{"scroll.all_pools"}}};
    ScrollBar scroll_bar_;
    std::optional<List> drag_list_;
    bool busy_ = false;
    bool navigation_open_ = true;
    Json presets_ = Json::object();

    static bool interactive(const Widget* w) {
        return w && w->enabled && (w->kind==WidgetKind::textbox || w->kind==WidgetKind::button ||
            w->kind==WidgetKind::select || w->visual==WidgetVisual::toggle_switch || ScrollBar::matches(*w));
    }
    ListViewport& viewport(List list) { return viewports_.at(static_cast<std::size_t>(list)); }
    const ListViewport& viewport(List list) const { return viewports_.at(static_cast<std::size_t>(list)); }
    std::optional<List> list_for(std::string_view key) const {
        for (const auto list:lists)
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
        if (list==List::enabled_pools) commit_concurrency();
        view.offset=offset;
        redraw();
    }
    void refresh_combo_layout() {
        rebuild_choice_layout(layout_,model_);
        InvalidateRect(window,nullptr,FALSE);
    }
    void combo_changed(const ComboBoxChange& change) {
        refresh_combo_layout();
        ComboBox::reveal_candidate(model_,layout_);
        refresh_combo_layout();
        if (!change.selection_changed) return;
        if (change.key.starts_with("schedule.")) save_schedule(change.key,change.new_value);
        else fields_[change.key]=widen(change.new_value);
    }

    void insert(std::wstring_view text) {
        auto* box = widget(model_,model_.focused);
        if (!box || box->kind!=WidgetKind::textbox || !box->enabled || box->read_only) return;
        if (TextEditor::insert(model_.ui,box->value,model_.focused,text,false,
            box->text_box?box->text_box->max_length:0))
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
        for (auto key : {"configPath","authPath","permission","preset"})
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
    void copy_agent_setup() {
        std::wstring executable(32768,L'\0');
        const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if (!length || length>=executable.size()) throw std::runtime_error("无法获取肥鱼重工程序路径");
        executable.resize(length);

        const Json config{{"mcpServers",{{"fatfishbox",{{"command",narrow(executable)},{"args",Json::array({"--mcp"})}}}}}};
        auto quoted=executable;
        for (std::size_t i=0;(i=quoted.find(L'\'',i))!=std::wstring::npos;i+=2) quoted.insert(i,1,L'\'');
        std::wstring setup=L"请接入这台 Windows 机器上的肥鱼重工（FatFishBox），服务名为 fatfishbox。";
        if (http_) {
            const auto url=widen(http_->url());
            setup+=L"支持 Streamable HTTP 的本机客户端可直接打开 "+url+
                L"，无需认证。Codex 可运行：\n```powershell\n"
                L"codex mcp add fatfishbox --url "+url+L"\n```\n\n";
        }
        setup+=L"也可以使用本地 stdio。按客户端支持的格式注册后，刷新工具列表。\n\n"
            L"stdio 配置示例：\n```json\n"+widen(config.dump(2))+
            L"\n```\n\n如果你是 Codex，可以在 PowerShell 运行：\n```powershell\n"
            L"codex mcp add fatfishbox -- '"+quoted+L"' --mcp\n```\n\n"
            L"接入后先调用 cluster_overview 验证连接。仅在用户授权时用 cluster_start_session 派发。"
            L"用 cluster_next_handoff({}) 按完成时间领取最早的待处理会话。完成实际处置后，"
            L"用同一工具提交 claimToken 和 outcome，确认当前结果后领取下一条。"
            L"只想重读当前结果时仅传 claimToken。需要返修时用 cluster_repair_handoff。"
            L"完整示例可读取 MCP 资源 fatfish://workflow。\n\n"
            L"如果你无法启动本机进程或修改 MCP 配置，请说明需要用户在客户端完成哪一步。";
        notice_=copy_text(setup)?L"已复制接入说明，粘贴给 Agent 即可":L"剪贴板暂不可用";
        redraw();
    }
    void local_snapshot() {
        ++snapshot_generation_;
        try {
            auto snapshot=cluster_.desktop_snapshot();
            if (!session_sync_at_) apply_sessions(snapshot);
            settings_=std::move(snapshot.at("settings"));
            update_pools();
        } catch (const std::exception& e) { notice_ = widen(e.what()); }
    }
    void apply_sessions(Json& snapshot) {
        const auto replace=[&](List list,Json& rows,const char* key) {
            auto& next=snapshot.at(key);
            auto& offset=viewport(list).offset;
            const auto first=static_cast<std::size_t>(std::lround(offset));
            if (first>0 && first<rows.size()) {
                const auto& id=rows.at(first).at("id");
                const auto same=std::ranges::find_if(next,[&](const Json& row){return row.at("id")==id;});
                if (same!=next.end()) offset=static_cast<double>(same-next.begin());
            }
            rows=std::move(next);
        };
        replace(List::running,running_,"running");
        replace(List::pending,pending_,"pending");
        overview_=std::move(snapshot.at("overview"));
        if (page_=="detail" && selected_.is_object()) {
            for (const auto* list:{&running_,&pending_}) {
                const auto current=std::ranges::find_if(*list,[&](const Json& row){return row.at("id")==selected_.at("id");});
                if (current==list->end()) continue;
                if (selected_.at("resultVersion")!=current->at("resultVersion")) clear_result();
                selected_=*current;
                break;
            }
        }
    }
    void update_pools() {
        account_models_.clear();
        for (const auto& [id,account]:settings_.at("accounts").items())
            account_models_.try_emplace(id);
        for (const auto& [id,pool]:settings_.at("pools").items())
            account_models_.at(pool.at("provider").get<std::string>()).push_back(id);
        std::erase_if(expanded_accounts_,[&](const auto& id){return !account_models_.contains(id);});
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
        if (page_=="sessions") refresh_sessions_automatically();
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
    std::wstring session_account(const Json& row) const {
        const auto provider=row.value("provider","");
        if (provider.empty()) return L"—";
        const auto accounts=settings_.find("accounts");
        if (accounts!=settings_.end()) {
            const auto account=accounts->find(provider);
            if (account!=accounts->end()) {
                const auto name=title(*account,"name");
                if (!name.empty()) return name;
            }
        }
        return widen(provider);
    }
    void session_section(List list,const Json& rows,float x,float y,float right,float bottom) {
        const bool running=list==List::running;
        const std::string surface=running?"running":"pending";
        const float left=x+16, table_right=right-32;
        const float width=table_right-left;
        const bool time_column=width>=650;
        const float time=table_right-92;
        const float state_width=width>=650?94.f:76.f;
        const float state=(time_column?time-8:table_right)-state_width;
        const float effort_column=state-72;
        const float name_columns=effort_column-left;
        const float account=left+name_columns*.51f;
        const float model=left+name_columns*.76f;
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
        label(L"会话",left,y+42,account-left-12,12,surface,quiet);
        label(L"账号",account,y+42,model-account-8,12,surface,quiet);
        label(L"模型",model,y+42,effort_column-model-8,12,surface,quiet);
        label(L"推理强度",effort_column,y+42,state-effort_column-8,12,surface,quiet);
        label(L"状态",state,y+42,state_width,12,surface,quiet);
        if (time_column) label(running?L"最近更新":L"完成时间",time,y+42,92,12,surface,quiet);
        if (!total) label(running?L"当前没有进行中的会话":L"当前没有已完成待处理的会话",
            left,y+73,width,14,surface,ColorToken::text_secondary);
        for (int i=start;i<end;++i) {
            const auto& row=rows.at(i);
            const float line=y+61+float(i-start)*row_height;
            const auto action="row."+std::to_string(visible_sessions_.size());
            visible_sessions_.push_back(row);
            table_row("session."+row.at("id").get<std::string>(),action,x+8,line,table_right-x,surface);
            label(title(row,"title"),left,line+7,account-left-12,14,surface);
            label(session_account(row),account,line+7,model-account-8,13,surface,ColorToken::text_secondary);
            auto model_name=title(row,"model");
            if (model_name.empty()) model_name=L"—";
            label(std::move(model_name),model,line+7,effort_column-model-8,13,surface,ColorToken::text_secondary);
            const auto effort=title(row,"effort");
            label(effort.empty()?L"—":effort,effort_column,line+7,state-effort_column-8,13,surface,ColorToken::text_secondary);
            label((row.value("stale",false)?L"◌ ":L"● ")+status(row),state,line+7,state_width,12,surface,status_color(row));
            if (time_column) label(local_stamp(row.value(running?"updated":"completed",std::int64_t(0))),
                time,line+7,92,12,surface,ColorToken::text_secondary);
        }
    }
    void session_page(float x,float right,float h) {
        std::wstring sync_state;
        auto color=ColorToken::text_secondary;
        if (sessions_worker_.joinable()) sync_state=L"正在同步会话…";
        else if (!sessions_error_.empty()) {sync_state=L"同步失败 · "+sessions_error_;color=ColorToken::status_warning;}
        else if (!sessions_timer_ready_) {sync_state=L"自动同步不可用";color=ColorToken::status_warning;}
        else if (!session_sync_at_) sync_state=L"等待首次同步";
        else {
            sync_state=L"最近同步 "+local_stamp(session_sync_at_,L"%H:%M:%S");
            if (session_live_stale_) {sync_state+=L" · "+std::to_wstring(session_live_stale_)+L" 条实时状态未确认";color=ColorToken::status_warning;}
            else if (session_live_checked_) sync_state+=L" · 已校验实时状态";
        }
        label(std::move(sync_state),right-418,27,302,12,"root",color);
        button("sync",L"同步会话",right-104,20,104);
        const auto usage=overview_.value("usage",Json::object());
        const auto total=usage.value("total",Json::object());
        const auto input=total.value("uncachedInputTokens",0.0), read=total.value("cacheReadTokens",0.0);
        const auto output=total.value("outputTokens",0.0), write=total.value("cacheWriteTokens",0.0);
        const auto billed=input+read+write;
        const auto hourly=usage.value("hourlyTokens",Json(nullptr));
        const auto amount=[&](double tokens) {
            return usage.value("sampled",0)>0 || !usage.value("missing",0)?compact_tokens(tokens):std::wstring(L"—");
        };
        const std::array<std::pair<const wchar_t*,std::wstring>,5> values{{
            {L"今日平均每小时",hourly.is_number()?compact_tokens(hourly.get<double>())+L" Token/h":L"—"},
            {L"未缓存输入",amount(input)},{L"输出",amount(output)},
            {L"缓存读取",amount(read)},{L"缓存命中",billed>0?fixed3(read*100/billed)+L"%":L"—"}
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
            if (usage.value("missing",0)>0) {
                if (!summary.empty()) summary+=L"  ·  ";
                summary+=usage_busy_?L"正在同步 Token 用量":std::to_wstring(usage.value("missing",0))+L" 个会话用量未同步";
            }
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
        button("refresh_presets",L"刷新预设",config_right-248,296,96,false,true,"dsh");
        button("save_connection",L"保存连接设置",config_right-142,296,126,true,true,"dsh");
        button("import",L"从 DSH 导入模型",right-154,20,154,true);

        const float mcp_x=side?config_right+12:x;
        const float mcp_y=side?64.f:358.f;
        panel("mcp",{mcp_x,mcp_y,right,mcp_y+(side?166.f:106.f)});
        label(L"MCP 连接",mcp_x+16,mcp_y+12,right-mcp_x-172,17,"mcp",ColorToken::text_primary,true);
        button("copy_agent_setup",L"复制接入说明",right-144,mcp_y+6,128,true,true,"mcp");
        if (http_) {
            label(L"本机 HTTP 监听中 · 无需认证",mcp_x+16,mcp_y+42,right-mcp_x-32,12,"mcp",ColorToken::status_success);
            label(widen(http_->url()),mcp_x+16,mcp_y+62,right-mcp_x-32,13,"mcp");
        } else label(http_error_,mcp_x+16,mcp_y+62,right-mcp_x-32,13,"mcp",ColorToken::status_error);
        if (side) label(L"stdio  ·  FatFishBox.exe --mcp",mcp_x+16,mcp_y+118,right-mcp_x-32,13,"mcp",ColorToken::text_secondary);
        else label(L"stdio  ·  FatFishBox.exe --mcp",mcp_x+16,mcp_y+83,right-mcp_x-32,12,"mcp",ColorToken::text_secondary);
    }
    void pools_section(List list,float x,float top,float right,float bottom) {
        const bool enabled_only=list==List::enabled_pools;
        const std::string surface=enabled_only?"enabled_pools":"all_pools";
        const auto& accounts=settings_.at("accounts");
        const auto& pools=settings_.at("pools");
        const float left=x+16;
        const float effort_width=std::clamp((right-x)*.23f,134.f,200.f), effort=right-32-effort_width;
        const float limit=effort-128, state=right-100;
        const float name_end=enabled_only?limit:state;
        const auto heading=enabled_only?L"已启用":L"完整列表";
        std::vector<std::pair<bool,std::string>> rows;
        int model_count=0;
        for (const auto& [account,models]:account_models_) {
            const auto enabled=[&](const std::string& id){return pools.at(id).at("enabled").get<bool>();};
            const auto active=std::ranges::count_if(models,enabled);
            if (enabled_only && !active) continue;
            model_count+=enabled_only?int(active):int(models.size());
            rows.emplace_back(false,account);
            for (const auto& id:models)
                if (enabled(id) || (!enabled_only && expanded_accounts_.contains(account)))
                    rows.emplace_back(true,id);
        }
        panel(surface,{x,top,right,bottom});
        label(heading,left,top+8,right-left-32,16,surface,ColorToken::text_primary,true);
        label(std::to_wstring(model_count)+L" 个模型",right-115,top+10,83,12,surface,ColorToken::text_secondary);
        const float first=top+57;
        const int total=int(rows.size()), capacity=std::max(1,int((bottom-first-7)/pool_row_height));
        const int start=list_start(list,surface,{x+8,first,right-7,bottom-8},total,capacity,L"滚动"+std::wstring(heading));
        label(L"账号 / 模型",left,top+33,name_end-left-12,12,surface,ColorToken::text_tertiary);
        if (enabled_only) {
            label(L"并发",limit,top+33,112,12,surface,ColorToken::text_tertiary);
            label(L"思考强度",effort,top+33,effort_width,12,surface,ColorToken::text_tertiary);
        } else label(L"启用",state,top+33,64,12,surface,ColorToken::text_tertiary);
        for (int position=start;position<std::min(start+capacity,total);++position) {
            const auto& [is_model,id]=rows[position];
            const float row_top=first+float(position-start)*pool_row_height;
            const float y=row_top+10;
            if (!is_model) {
                auto name=title(accounts.at(id),"name");
                const auto& children=account_models_.at(id);
                const auto active=std::ranges::count_if(children,[&](const auto& model){return pools.at(model).at("enabled").get<bool>();});
                if (enabled_only || static_cast<std::size_t>(active)==children.size()) {
                    label(std::move(name),left+26,y+7,name_end-left-38,14,surface,ColorToken::text_primary,true);
                } else {
                    name+=L"  ·  "+std::to_wstring(active)+L" / "+std::to_wstring(children.size())+L" 已启用";
                    auto& header=add("expand."+id,WidgetKind::button,std::move(name),left,y,name_end-left-12,32,surface);
                    header.visual=WidgetVisual::tree_item;
                    header.hierarchy=HierarchyInfo{0,expanded_accounts_.contains(id),true,false};
                    header.enabled=!busy_;
                }
                continue;
            }
            const auto& row=pools.at(id);
            label(title(row,"name"),left+44,y+7,name_end-left-56,14,surface,ColorToken::text_secondary);
            if (!enabled_only) {
                auto& toggle=ToggleSwitch({.on=row.at("enabled").get<bool>(),.enabled=!busy_})
                    .append(model_,surface,"schedule.enabled."+id);
                toggle.action=toggle.key;
                toggle.area=WidgetArea::absolute;
                const float toggle_top=row_top-14+title_bar_.height();
                toggle.bounds={state,toggle_top,state+52,toggle_top+pool_row_height};
                continue;
            }
            const auto concurrency=row.at("concurrency").get<int>();
            const auto binding="schedule.concurrency."+id;
            const auto draft=fields_.find(binding);
            const auto value=draft==fields_.end()?std::to_wstring(concurrency):draft->second;
            button("lower."+id,L"−",limit,y,26,false,concurrency>1,surface);
            auto& number_box=add(binding,WidgetKind::textbox,L"",limit+30,y,52,32,surface);
            TextBox({.text=value,.enabled=!busy_,.max_length=4}).apply(number_box);
            number_box.binding=binding;
            button("raise."+id,L"+",limit+86,y,26,false,concurrency<1000,surface);
            std::vector<Choice> options{{"",L"DSH 默认"}};
            const auto saved=row.at("effort").get<std::string>();
            for (const auto& option:row.at("efforts")) options.push_back({option.get<std::string>(),widen(option.get<std::string>())});
            if (std::ranges::find(options,saved,&Choice::key)==options.end())
                options.push_back({saved,widen(saved)+L"（已下架）",false});
            auto& choice=add("schedule.effort."+id,WidgetKind::select,L"",effort,y,effort_width,32,surface);
            ComboBox({L"思考强度",std::move(options),widen(saved),!busy_,effort_width}).apply(choice);
        }
        if (!total) label(enabled_only?L"在下方完整列表启用模型后，可在这里编辑配置。":L"从 DSH 导入模型后可在这里启用。",
            left,first+8,right-left-32,13,surface,ColorToken::text_secondary);
    }
    void pools_page(float x,float right,float h) {
        button("import",L"从 DSH 导入模型",right-154,20,154,true);
        constexpr float top=65,gap=12;
        const float bottom=h-30;
        const float split=top+std::floor((bottom-top-gap)*.49f);
        pools_section(List::enabled_pools,x,top,right,split);
        pools_section(List::all_pools,x,split+gap,right,bottom);
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
        const auto selected_page=page_=="pools" ? "pools" :
            page_=="connection" ? "connection" : "sessions";
        NavigationViewOptions navigation;
        navigation.id="sidebar";
        navigation.pane_expanded=navigation_open_ && w>=minimum_width;
        navigation.selected_id=selected_page;
        navigation.items={{"sessions",L"会话",L"\uE8BD",!busy_,{}},
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
        const std::map<std::string,std::wstring> names{{"sessions",L"会话"},{"pools",L"账号与模型池"},
            {"connection",L"连接与 MCP"},{"detail",L"会话详情"}};
        label(names.at(page_),x,19,width-246,25);
        if (!notice_.empty()) label(notice_,x,h-24,width,12,"root",ColorToken::text_secondary);
        if (page_=="sessions") {
            session_page(x,right,h);
        } else if (page_=="connection") {
            connection_page(x,right);
        } else if (page_=="pools") {
            pools_page(x,right,h);
        } else if (page_=="detail" && selected_.is_object()) {
            button("nav.sessions",L"返回会话",right-110,20,110);
            const float left=x+16, inner=width-32;
            panel("session_detail",{x,64,right,219});
            label(title(selected_,"title"),left,76,inner,19,"session_detail",ColorToken::text_primary,true);
            const auto batch=title(selected_,"batch"), version=title(selected_,"resultVersion");
            label(status(selected_)+(selected_.value("stale",false)?L"（登记或上次状态）":L"")+
                (batch.empty()?L"":L"  ·  批次 "+batch)+(version.empty()?L"":L"  ·  结果版本 "+version),
                left,107,inner,13,"session_detail",status_color(selected_));
            label(L"工作区  "+title(selected_,"workspace"),left,132,inner,13,"session_detail",ColorToken::text_secondary);
            const auto model_name=title(selected_,"model"),effort=title(selected_,"effort");
            label(L"账号  "+session_account(selected_)+L"    模型  "+(model_name.empty()?L"—":model_name)+
                L"    推理强度  "+(effort.empty()?L"—":effort),left,152,inner,13,"session_detail",ColorToken::text_secondary);
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
    void commit_concurrency() {
        const auto* box=widget(model_,model_.focused);
        if (!box || !box->binding.starts_with("schedule.concurrency.")) return;
        const auto action=box->binding;
        const auto draft=fields_.find(action);
        if (draft==fields_.end()) return;
        std::size_t length=0;
        int value=0;
        try {value=std::stoi(draft->second,&length);}
        catch (const std::exception&) {}
        if (value<1 || value>1000 || length!=draft->second.size()) {
            fields_.erase(draft);
            notice_=L"并发必须是 1 至 1000 的整数，已恢复上次保存的值";
            redraw();
            return;
        }
        save_schedule(action,value);
    }
    void save_schedule(std::string action,Json value) {
        if (busy_) return;
        fields_.erase(action);
        try {
            constexpr auto prefix=std::string_view("schedule.").size();
            const auto split=action.find('.',prefix);
            const auto field=action.substr(prefix,split-prefix);
            const auto id=action.substr(split+1);
            const auto expected=settings_.at("pools").at(id).at(field);
            settings_=cluster_.update_model_field(id,field,expected,value);
            update_pools();
            notice_=L"模型配置已保存";
        } catch (const std::exception& error) {
            notice_=L"保存失败 · "+widen(error.what());
            try {settings_=cluster_.invoke("cluster_get_settings");update_pools();}
            catch (const std::exception&) {}
        }
        redraw();
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
        if (w->visual==WidgetVisual::toggle_switch) {
            save_schedule(w->action,!w->selected);
            return;
        }
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
            if (name.starts_with("expand.")) {
                const auto id=name.substr(7);
                if (!expanded_accounts_.erase(id)) expanded_accounts_.insert(id);
                redraw();
                return;
            }
            if (name=="refresh_presets") {refresh_presets();return;}
            if (name=="copy_agent_setup") {copy_agent_setup();return;}
            if (name=="copy_result") {
                notice_=copy_text(result_loaded_?output_:title(selected_,"error"))?L"已复制结果":L"剪贴板暂不可用";
                redraw();return;
            }
            if (name.starts_with("raise.") || name.starts_with("lower.")) {
                const auto id=name.substr(6);
                save_schedule("schedule.concurrency."+id,
                    settings_.at("pools").at(id).at("concurrency").get<int>()+(name.starts_with("raise.")?1:-1));
                return;
            }
            if (name.starts_with("row.")) {selected_=visible_sessions_.at(std::stoul(name.substr(4)));clear_result();nav("detail");return;}
            if (name=="sync") {
                refresh_sessions_automatically(true);
            } else if (name=="import") {
                call("cluster_import_models",page_=="connection"?paths():Json::object(),[this](Json value) {
                    std::erase_if(fields_,[](const auto& entry){return entry.first.starts_with("schedule.");});
                    local_snapshot();nav("pools");notice_=L"已导入 "+std::to_wstring(value.value("accounts",0))+L" 个账号、"+std::to_wstring(value.value("models",0))+L" 个模型";
                });
            } else if (name=="save_connection") {
                Json input=paths();
                input.update(Json{{"revision",settings_.at("revision")},{"permission",narrow(fields_.at("permission"))},
                    {"preset",narrow(fields_.at("preset"))}});
                call("cluster_configure",input,[this](Json value){settings_=std::move(value);local_snapshot();notice_=L"连接设置已保存";});
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
    case WM_KEYUP: app->key_up(wp);break;
    case WM_SYSKEYDOWN: if ((wp==VK_UP || wp==VK_DOWN) && app->key(wp)) return 0;break;
    case WM_CHAR: app->type(static_cast<wchar_t>(wp));return 0;
    case finished: app->receive();return 0;
    case usage_finished: app->receive_usage();return 0;
    case sessions_finished: app->receive_sessions();return 0;
    case WM_TIMER:
        if (wp==usage_timer) { app->sync_usage();return 0; }
        if (wp==sessions_timer) { app->refresh_sessions_automatically();return 0; }
        break;
    case WM_CLOSE: app->closing();break;
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
