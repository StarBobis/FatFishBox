#include "diagnostics.h"
#include "result_preview.h"
#include "cluster.h"
#include "dsh.h"
#include "legacy.h"
#include "mcp.h"
#include "team.h"
#include "dsh.h"
#include "resource.h"
#include <native_ui/button.h>
#include <native_ui/combo_box.h>
#include <native_ui/expander.h>
#include <native_ui/image.h>
#include <native_ui/navigation_view.h>
#include <native_ui/pivot.h>
#include <native_ui/renderer.h>
#include <native_ui/scroll_bar.h>
#include <native_ui/text_block.h>
#include <native_ui/text_box.h>
#include <native_ui/theme_resources.h>
#include <native_ui/title_bar.h>
#include <native_ui/toggle_switch.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <fstream>
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
constexpr UINT detail_finished = WM_APP + 4;
constexpr UINT tray_notification = WM_APP + 5;
constexpr UINT tray_icon_id = 1;
constexpr UINT_PTR usage_timer = 1;
constexpr UINT_PTR sessions_timer = 2;
constexpr UINT sessions_interval_ms = 5000;
constexpr int minimum_width=740, minimum_height=520;
constexpr float row_height=34;
constexpr float pool_row_height=52;
enum class List { running, pending, processed, enabled_pools, all_pools, settings, teams };
constexpr std::array lists{List::running,List::pending,List::processed,List::enabled_pools,List::all_pools,List::settings,List::teams};
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
    if (row.value("handoffClaimed",false)) return L"已领取";
    const auto review=row.value("reviewState","");
    if (review=="passed") return L"初步核验通过";
    if (review=="needs_repair") return L"审查未通过";
    if (review=="blocked") return L"审查受阻";
    if (review=="reviewing") return L"对抗审查中";
    if (review=="overdue") return L"审查耗时过长";
    if (review=="scheduling" || (review=="pending" && !row.value("resultVersion","").empty())) return L"等待对抗审查";
    if (row.value("status","")=="unknown" && row.value("dispatchState","")=="unconfirmed")
        return row.contains("dispatchRetryAt")?L"派发重试中":L"派发未确认";
    if (row.value("resultVersion","")=="dispatch:failed") return L"派发失败";
    static const std::map<std::string,std::wstring> labels{
        {"starting",L"启动中"},{"queued",L"待同步"},{"running",L"运行中"},{"waiting_input",L"等待输入"},
        {"cancelling_policy_violation",L"正在停止"},
        {"succeeded",L"完成"},{"cancelled",L"取消"},{"failed",L"失败"},{"unknown",L"待同步"},{"interrupted",L"中断"}};
    auto found = labels.find(row.value("status",""));
    return found == labels.end() ? L"—" : found->second;
}
ColorToken status_color(const Json& row) {
    const auto state=row.value("status","");
    const auto review=row.value("reviewState","");
    if (row.value("stale",false)) return ColorToken::status_warning;
    if (row.value("handoffClaimed",false)) return ColorToken::accent_primary;
    if (review=="needs_repair") return ColorToken::status_error;
    if (review=="blocked" || review=="overdue") return ColorToken::status_warning;
    if (review=="reviewing" || review=="scheduling") return ColorToken::accent_primary;
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
        detail_worker_.request_stop();
        if (usage_worker_.joinable()) usage_worker_.join();
        if (sessions_worker_.joinable()) sessions_worker_.join();
        if (detail_worker_.joinable()) detail_worker_.join();
        if (worker_.joinable()) worker_.join();
    }
    void initialize_view() {
        model_.theme = Theme::dark;
        model_.theme_resources = desktop_theme();
        logo_ = embedded_logo();
        completed_tabs_.set_items({{"pending",L"待处理"},{"processed",L"已确认"}});
    }
    void init() {
        initialize_view();
        renderer_.initialize();
        renderer_.set_dpi(GetDpiForWindow(window));
        try {cluster_.migrate_repair_queue();}
        catch (const std::exception& error) { fatfish::diagnostics::caught();notice_=widen(error.what());}
        try {
            close_behavior_=cluster_.invoke("cluster_desktop_close_behavior").at("closeBehavior").get<std::string>();
            expanded_settings_=cluster_.invoke("cluster_desktop_settings_expanders");
        } catch (const std::exception& error) { fatfish::diagnostics::caught(); notice_=widen(error.what()); }
        fields_["closeBehavior"]=widen(close_behavior_);
        taskbar_created_=RegisterWindowMessageW(L"TaskbarCreated");
        add_tray_icon();
        try { http_=std::make_unique<fatfish::McpHttpServer>(cluster_); }
        catch (const std::exception& error) { fatfish::diagnostics::caught(); http_error_=widen(error.what()); }
        redraw();
        sync_usage();
        reload();
        SetTimer(window,usage_timer,60000,nullptr);
        sessions_timer_ready_=SetTimer(window,sessions_timer,sessions_interval_ms,nullptr)!=0;
        refresh_sessions_automatically();
    }
    void render_offscreen(std::string_view page, const fatfish::fs::path& output, UINT width, UINT height, UINT dpi) {
        initialize_view();
        if (FAILED(renderer_.initialize())) throw std::runtime_error("离屏渲染器初始化失败");
        renderer_.set_dpi(dpi);
        page_=page=="settings"?"connection":page;
        expanded_settings_={{"general",true},{"dsh",false},{"team",true},{"mcp",true}};
        close_behavior_=cluster_.invoke("cluster_desktop_close_behavior").at("closeBehavior").get<std::string>();
        fields_["closeBehavior"]=widen(close_behavior_);
        local_snapshot();
        if (!notice_.empty()) throw std::runtime_error(narrow(notice_));
        http_error_=L"离屏预览";
        const DpiScale scale(dpi);
        build_view(float(scale.dips(width)),float(scale.dips(height)),false);
        std::vector<unsigned char> png;
        if (FAILED(renderer_.capture_offscreen(width,height,dpi,model_,layout_,Theme::dark,png)))
            throw std::runtime_error("离屏渲染失败");
        std::ofstream file(output,std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));
        file.close();
        if (!file) throw std::runtime_error("无法写入离屏 PNG");
    }
    void size(int width, int height) {
        if (!width || !height) return;
        renderer_.resize(static_cast<UINT>(width), static_cast<UINT>(height));
        redraw();
    }
    void dpi(UINT value) { renderer_.set_dpi(value); redraw(); }
    void paint() { renderer_.paint(window, model_, layout_, Theme::dark); }
    void closing() {
        if (close_prompt_open_) return;
        commit_concurrency();
        if (close_behavior_=="tray") { minimize_to_tray(); return; }
        if (close_behavior_=="exit") { DestroyWindow(window); return; }

        constexpr int to_tray=101, exit_app=102;
        const TASKDIALOG_BUTTON choices[]{{to_tray,L"进入托盘"},{exit_app,L"直接退出"}};
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize=sizeof(dialog);
        dialog.hwndParent=window;
        dialog.hInstance=GetModuleHandleW(nullptr);
        dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;
        dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;
        dialog.pszWindowTitle=L"肥鱼重工";
        dialog.pszMainIcon=MAKEINTRESOURCEW(IDI_FATFISHBOX);
        dialog.pszMainInstruction=L"关闭窗口时怎么处理？";
        dialog.pszContent=L"进入托盘后，肥鱼重工和本机 MCP 服务会继续运行。";
        dialog.cButtons=static_cast<UINT>(std::size(choices));
        dialog.pButtons=choices;
        dialog.nDefaultButton=to_tray;
        dialog.pszVerificationText=L"记住我的选择";
        int action=IDCANCEL;
        BOOL remember=FALSE;
        close_prompt_open_=true;
        const auto shown=TaskDialogIndirect(&dialog,&action,nullptr,&remember);
        close_prompt_open_=false;
        if (FAILED(shown)) {
            notice_=L"无法打开关闭提示。可在设置中选择关闭行为。";
            redraw();
            return;
        }
        if (action!=to_tray && action!=exit_app) return;
        if (remember && !save_close_behavior(action==to_tray?"tray":"exit")) return;
        if (action==to_tray) minimize_to_tray();
        else DestroyWindow(window);
    }
    void remove_tray_icon() {
        if (!tray_registered_) return;
        auto icon=tray_icon();
        Shell_NotifyIconW(NIM_DELETE,&icon);
        tray_registered_=false;
    }
    bool taskbar_recreated(UINT message) {
        if (!taskbar_created_ || message!=taskbar_created_) return false;
        tray_registered_=false;
        add_tray_icon();
        if (in_tray_ && !tray_registered_) restore_window();
        return true;
    }
    void tray_event(WPARAM position,LPARAM event) {
        if (close_prompt_open_) return;
        const auto notification=LOWORD(event);
        if (notification==NIN_SELECT || notification==NIN_KEYSELECT || notification==WM_LBUTTONUP || notification==WM_LBUTTONDBLCLK) {
            restore_window();
        } else if (notification==WM_CONTEXTMENU || notification==WM_RBUTTONUP) {
            POINT point{GET_X_LPARAM(position),GET_Y_LPARAM(position)};
            if (notification==WM_RBUTTONUP || (point.x==-1 && point.y==-1)) GetCursorPos(&point);
            show_tray_menu(point);
        }
    }
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
    void session_context(int x,int y) {
        const auto* control=widget(model_,hit(x,y));
        if (!control || !control->action.starts_with("row.")) return;
        const auto row=visible_sessions_.at(std::stoul(control->action.substr(4)));
        if (row.value("readOnly",false) || !row.value("resultVersion","").empty()) return;
        HMENU menu=CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu,MF_STRING,1,L"标记为失败");
        POINT point{x,y}; ClientToScreen(window,&point);
        const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);
        DestroyMenu(menu);
        if (command!=1) return;
        const auto key=row.at("id").get<std::string>(),queue=current_queue_;
        job([this,key,queue] {return cluster_.for_queue(queue).mark_desktop_failed(key);},[this](Json) {
            local_snapshot();
            notice_=L"已标记为失败，会话已进入待处理";
        });
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
                if (interactive(&w) && (w.kind!=WidgetKind::tab || w.tab_stop))
                    controls.push_back(widget_base + int(i));
            }
            if (controls.empty()) return true;
            auto current = std::find(controls.begin(),controls.end(),model_.focused);
            const int n = int(controls.size());
            const int step = (GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
            int index = current == controls.end() ? (step > 0 ? 0 : n - 1) : (int(current-controls.begin()) + n + step) % n;
            model_.focused = controls[index];
            model_.focus_visible = true;
            if (const auto* control=widget(model_,model_.focused); control && control->surface.starts_with("settings.")) {
                const auto& view=viewport(List::settings);
                const auto bounds=control_bounds(model_.focused);
                if (bounds.top<view.bounds.top)
                    move_list(List::settings,view.offset+(bounds.top-view.bounds.top)/row_height);
                else if (bounds.bottom>view.bounds.bottom)
                    move_list(List::settings,view.offset+(bounds.bottom-view.bounds.bottom)/row_height);
            }
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
        if (box->kind==WidgetKind::tab) {
            const std::map<WPARAM,std::string_view> keys{{VK_LEFT,"Left"},{VK_RIGHT,"Right"},
                {VK_HOME,"Home"},{VK_END,"End"},{VK_RETURN,"Enter"},{VK_SPACE,"Space"}};
            if (const auto key=keys.find(pressed); key!=keys.end() &&
                completed_tabs_.handle_key(model_,layout_,model_.focused,key->second)) {
                redraw();
                return true;
            }
        }
        if (Expander::is_header(*box) && (pressed==VK_RETURN || pressed==VK_SPACE)) {
            click(model_.focused);
            return true;
        }
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
                auto completed=std::exchange(result_,std::nullopt);
                auto callback=std::move(callback_);
                if (completed && callback) callback(std::move(*completed));
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                notice_=widen(error.what());
            }
        } else {
            notice_ = error_;
            if (page_=="pools") {
                try { settings_=cluster_.invoke("cluster_get_settings");update_pools(); }
                catch (const std::exception&) { fatfish::diagnostics::caught();}
            }
        }
        redraw();
    }
    void sync_usage() {
        if (usage_busy_) return;
        if (usage_worker_.joinable()) usage_worker_.join();
        usage_busy_=true;
        usage_worker_=std::jthread([this](std::stop_token stop) {
            try { cluster_.refresh_usage(stop); }
            catch (const std::exception&) { fatfish::diagnostics::caught();}
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
        if ((page_!="sessions" && page_!="detail") || current_queue_.empty()) return;
        if (manual) {
            sync_usage();
            refresh_requested_=sessions_worker_.joinable();
        }
        if (IsIconic(window) || sessions_worker_.joinable()) return;
        if (sessions_snapshot_) apply_session_refresh();
        sessions_generation_=snapshot_generation_;
        const auto scoped=cluster_.for_queue(current_queue_);
        sessions_worker_=std::jthread([this,scoped](std::stop_token stop) {
            try {
                sessions_snapshot_=scoped.desktop_snapshot(&desktop_monitor_,stop);
                (*sessions_snapshot_)["teamQueue"]=cluster_.invoke("team_get_queue",{{"queueId",sessions_snapshot_->at("queueId")}});
                sessions_error_.clear();
            } catch (...) { fatfish::diagnostics::caught(); sessions_error_=L"会话同步失败，详情见 logs 日志"; }
            if (!stop.stop_requested()) PostMessageW(window,sessions_finished,0,0);
        });
        if (GetCapture()!=window) redraw();
    }
    void receive_sessions() {
        if (sessions_worker_.joinable()) sessions_worker_.join();
        apply_session_refresh();
        if (std::exchange(refresh_requested_,false)) refresh_sessions_automatically();
    }
    void receive_detail_result() {
        if (detail_worker_.joinable()) detail_worker_.join();
        auto response=std::exchange(detail_reply_,std::nullopt);
        auto error=std::exchange(detail_worker_error_,std::wstring{});
        if (detail_worker_generation_!=detail_generation_) {
            if (result_loading_) read_detail_result();
            return;
        }
        result_loading_=false;
        detail_read_error_=std::move(error);
        if (response) {
            if (response->contains("session")) selected_=std::move(response->at("session"));
            for (const auto& message:response->at("messages")) {
                if (message.value("type","")=="dispatch/error") continue;
                if (!output_.empty()) output_+=L"\r\n\r\n";
                output_+=title(message,"text");
            }
            failure_=response->value("failure",Json::object());
            detail_read_error_=title(*response,"readError");
            detail_from_history_=response->value("resultSource","")=="dsh_history";
            result_loaded_=true;
        }
        redraw();
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
    Pivot completed_tabs_{{.header_height=34,.header_padding=24,.header_gap=12}};
    std::shared_ptr<ImageSource> logo_;
    UINT taskbar_created_=0;
    bool tray_registered_=false,in_tray_=false,restore_maximized_=false,close_prompt_open_=false;
    std::string close_behavior_="ask";
    Json expanded_settings_={{"general",true}};
    std::map<std::string,std::wstring> fields_;
    Json running_=Json::array(), pending_=Json::array(), processed_=Json::array();
    Json settings_=Json::object(), overview_=Json::object(), selected_;
    Json team_settings_=Json::object(),queue_team_=Json::object();
    bool queue_progress_expanded_=false,show_leads_=false,compact_completed_=false;
    Json dsh_profiles_=Json::object(), detected_dsh_=Json::array();
    std::string active_dsh_profile_, draft_dsh_profile_;
    std::wstring detected_auth_;
    bool automatic_auth_{};
    Json queues_=Json::array();
    std::string current_queue_;
    std::map<std::string,std::vector<std::string>> account_models_;
    std::set<std::string> expanded_accounts_;
    std::vector<Json> visible_sessions_;
    std::jthread worker_;
    std::jthread usage_worker_;
    std::jthread sessions_worker_;
    std::jthread detail_worker_;
    std::optional<Json> detail_reply_;
    std::wstring detail_worker_error_,detail_read_error_;
    bool detail_from_history_=false;
    std::uint64_t detail_generation_=0,detail_worker_generation_=0;
    Json failure_=Json::object();
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
    bool result_loaded_=false,result_loading_=false;
    std::string page_ = "sessions";
    std::array<ListViewport,7> viewports_{{{"scroll.running"},{"scroll.pending"},{"scroll.processed"},
        {"scroll.enabled_pools"},{"scroll.all_pools"},{"scroll.settings"},{"scroll.teams"}}};
    ScrollBar scroll_bar_;
    std::optional<List> drag_list_;
    bool busy_ = false;
    bool navigation_open_ = true;
    Json presets_ = Json::object();

    static bool interactive(const Widget* w) {
        return w && w->enabled && (w->kind==WidgetKind::textbox || w->kind==WidgetKind::button ||
            w->kind==WidgetKind::select || w->kind==WidgetKind::tab || Expander::is_header(*w) ||
            w->visual==WidgetVisual::toggle_switch || ScrollBar::matches(*w));
    }
    NOTIFYICONDATAW tray_icon() const {
        NOTIFYICONDATAW icon{};
        icon.cbSize=sizeof(icon);
        icon.hWnd=window;
        icon.uID=tray_icon_id;
        return icon;
    }
    void add_tray_icon() {
        if (tray_registered_) return;
        auto icon=tray_icon();
        icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
        icon.uCallbackMessage=tray_notification;
        icon.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_FATFISHBOX));
        wcscpy_s(icon.szTip,L"肥鱼重工");
        tray_registered_=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
        if (tray_registered_) {
            icon.uVersion=NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION,&icon);
        }
    }
    void minimize_to_tray() {
        if (in_tray_) return;
        add_tray_icon();
        if (!tray_registered_) {
            notice_=L"托盘暂不可用，窗口已最小化到任务栏。";
            redraw();
            ShowWindow(window,SW_MINIMIZE);
            return;
        }
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(window,&placement);
        restore_maximized_=IsZoomed(window) || (placement.flags&WPF_RESTORETOMAXIMIZED)!=0;
        in_tray_=true;
        ShowWindow(window,SW_HIDE);
    }
    void restore_window() {
        const auto command=in_tray_ && restore_maximized_?SW_SHOWMAXIMIZED:SW_RESTORE;
        if (in_tray_ || IsIconic(window) || !IsWindowVisible(window)) ShowWindow(window,command);
        in_tray_=false;
        SetForegroundWindow(window);
    }
    void show_tray_menu(POINT point) {
        const auto menu=CreatePopupMenu();
        if (!menu) return;
        enum Command { open=1, sync, copy, settings, hide, exit };
        const auto ready=MF_STRING|(busy_?MF_GRAYED:MF_ENABLED);
        AppendMenuW(menu,MF_STRING,open,L"打开肥鱼重工");
        SetMenuDefaultItem(menu,open,FALSE);
        AppendMenuW(menu,ready,sync,L"同步会话");
        AppendMenuW(menu,MF_STRING|(current_queue_.empty()?MF_GRAYED:MF_ENABLED),copy,L"复制接入说明");
        AppendMenuW(menu,ready,settings,L"设置");
        AppendMenuW(menu,MF_STRING|(in_tray_?MF_GRAYED:MF_ENABLED),hide,L"进入托盘");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,exit,L"退出");
        SetForegroundWindow(window);
        const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);
        DestroyMenu(menu);
        PostMessageW(window,WM_NULL,0,0);
        switch (selected) {
        case open: restore_window(); break;
        case sync: restore_window(); nav("sessions"); refresh_sessions_automatically(true); break;
        case copy:
            try { copy_agent_setup(); }
            catch (const std::exception& error) { fatfish::diagnostics::caught(); restore_window(); notice_=widen(error.what()); redraw(); }
            break;
        case settings: restore_window(); nav("connection"); break;
        case hide: commit_concurrency(); minimize_to_tray(); break;
        case exit: commit_concurrency(); DestroyWindow(window); break;
        }
    }
    bool save_close_behavior(const std::string& value) {
        try {
            close_behavior_=cluster_.invoke("cluster_desktop_close_behavior",{{"closeBehavior",value}}).at("closeBehavior").get<std::string>();
            fields_["closeBehavior"]=widen(close_behavior_);
            return true;
        } catch (const std::exception& error) { fatfish::diagnostics::caught();
            notice_=L"关闭行为保存失败 · "+widen(error.what());
            redraw();
            return false;
        }
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
        else if (change.key=="closeBehavior") {
            if (save_close_behavior(change.new_value)) notice_=L"关闭行为已保存";
            redraw();
        }
        else if (change.key=="queueId") {
            try { switch_queue(change.new_value); }
            catch (const std::exception& error) { fatfish::diagnostics::caught(); notice_=widen(error.what()); redraw(); }
        }
        else if (change.key=="dshProfile") {
            select_dsh_profile(change.new_value);
            refresh_presets();
            redraw();
        }
        else if (change.key=="queueMode") {
            try {queue_team_=cluster_.invoke("team_desktop_queue",{{"queueId",current_queue_},{"mode",change.new_value}});fields_[change.key]=widen(queue_team_.at("mode").get<std::string>());}
            catch (const std::exception& error) {fatfish::diagnostics::caught();notice_=widen(error.what());fields_[change.key]=widen(queue_team_.value("mode","direct"));}
            redraw();
        }
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
            catch (const std::exception& e) { fatfish::diagnostics::caught();
                try { error_ = widen(e.what()); } catch (...) { fatfish::diagnostics::caught(); error_ = L"操作失败"; }
            }
            catch (...) { fatfish::diagnostics::caught(); error_=L"操作失败，详情见 logs 日志"; }
            PostMessageW(window,finished,0,0);
        });
    }
    void call(std::string operation, Json input, std::function<void(Json)> after) {
        const auto scoped=current_queue_.empty()?cluster_:cluster_.for_queue(current_queue_);
        job([scoped, operation=std::move(operation), input=std::move(input)] { return scoped.invoke(operation,input); },
            std::move(after));
    }
    void set_dsh_draft(const Json& row) {
        for (auto key : {"configPath","authPath","permission","preset"})
            fields_[key]=title(row,key);
        fields_["dshName"]=title(row,"name");
        automatic_auth_=row.value("automaticAuth",false);
        detected_auth_=fields_.at("authPath");
        presets_=Json::object();
    }
    void select_dsh_profile(const std::string& choice) {
        fields_["dshProfile"]=widen(choice);
        draft_dsh_profile_=choice;
        if (choice=="new") {
            set_dsh_draft(Json::object());
        } else if (choice.starts_with("detected:")) {
            const auto& candidate=detected_dsh_.at(std::stoul(choice.substr(9)));
            draft_dsh_profile_="new";
            set_dsh_draft(candidate);
            for (const auto& [id,saved]:dsh_profiles_.items()) {
                const auto same=[&](const char* key) {
                    return _wcsicmp(title(saved,key).c_str(),title(candidate,key).c_str())==0;
                };
                const auto services=[](const std::wstring& path) {
                    return std::filesystem::path(path).parent_path().parent_path();
                };
                const auto old_services=services(title(saved,"authPath"));
                const auto new_services=services(title(candidate,"authPath"));
                const bool same_managed=candidate.value("automaticAuth",false) &&
                    _wcsicmp(new_services.filename().c_str(),L"services")==0 &&
                    _wcsicmp(old_services.c_str(),new_services.c_str())==0;
                if (same("configPath") && (same("authPath") || same_managed ||
                    (!fields_.at("configPath").empty() && saved.value("automaticAuth",false)))) {
                    draft_dsh_profile_=id;
                    if (saved.at("name")!="") fields_["dshName"]=title(saved,"name");
                    for (const auto key:{"permission","preset"})
                        if (saved.at(key)!="") fields_[key]=title(saved,key);
                    break;
                }
            }
        } else set_dsh_draft(dsh_profiles_.at(choice));
    }
    void load_connection(std::wstring success_notice={}) {
        auto list=cluster_.invoke("cluster_list_dsh_profiles");
        dsh_profiles_=std::move(list.at("profiles"));
        active_dsh_profile_=list.at("current").get<std::string>();
        select_dsh_profile(active_dsh_profile_);
        if (expanded_settings_.value("dsh",false)) refresh_presets(std::move(success_notice));
        else if (!success_notice.empty()) notice_=std::move(success_notice);
    }
    void show_dsh_detection(Json candidates,bool after_authorization=false) {
        detected_dsh_=std::move(candidates);
        const auto reachable=std::ranges::count_if(detected_dsh_,[](const Json& row){return row.at("reachable")==true;});
        const auto desktop=std::ranges::find_if(detected_dsh_,[](const Json& row){
            return row.value("desktopPlugin",false) && row.value("reachable",false);
        });
        if (after_authorization && desktop!=detected_dsh_.end()) {
            select_dsh_profile("detected:"+std::to_string(desktop-detected_dsh_.begin()));
            refresh_presets(L"DSH 桌面版已授权。可选择预设，再保存或导入。");
            return;
        }
        const bool selected=reachable==1 || detected_dsh_.size()==1;
        const bool restored=!selected && narrow(fields_.at("dshProfile")).starts_with("detected:");
        if (selected) select_dsh_profile("detected:0");
        else if (restored) select_dsh_profile(active_dsh_profile_);
        notice_=detected_dsh_.empty()?L"未找到 DSH 配置，可手动添加。":
            L"找到 "+std::to_wstring(detected_dsh_.size())+L" 份配置，"+
            std::to_wstring(reachable)+L" 份可连接。可从下拉列表选择。";
        if (std::ranges::any_of(detected_dsh_,[](const Json& row){return row.value("setupRequired",false);}))
            notice_=L"已找到 DSH 桌面版。点击「自动授权」即可安装接入插件并验证连接。";
        if ((selected && reachable==1) || restored) refresh_presets(notice_);
    }
    void detect_dsh_profiles() {
        call("cluster_detect_dsh_connection",paths(),[this](Json candidates) {
            show_dsh_detection(std::move(candidates));
        });
    }
    bool dsh_draft_needs_authorization() const {
        return std::ranges::any_of(detected_dsh_,[this](const Json& row){
            return row.value("setupRequired",false) && fields_.at("configPath").empty() &&
                _wcsicmp(title(row,"authPath").c_str(),fields_.at("authPath").c_str())==0;
        });
    }
    void authorize_desktop() {
        job([cluster=cluster_,input=paths()] {
            fatfish::authorize_dsh_desktop();
            return cluster.invoke("cluster_detect_dsh_connection",input);
        },[this](Json candidates) { show_dsh_detection(std::move(candidates),true); });
        notice_=L"正在授权。若 DSH 仍在运行，请从它的「应用」菜单退出，肥鱼重工会继续安装。";
        redraw();
    }
    void refresh_presets(std::wstring success_notice={}) {
        const auto input=paths();
        if ((draft_dsh_profile_=="new" && input.at("configPath")=="" && input.at("authPath")=="") ||
            dsh_draft_needs_authorization()) return;
        call("cluster_get_presets",input,[this,success_notice=std::move(success_notice)](Json data) {
            presets_=std::move(data);
            if (!presets_.value("connected",false)) notice_=L"设置 DSH 连接后可读取完整预设";
            else if (!success_notice.empty()) notice_=success_notice;
            else notice_=L"已刷新 DSH 预设";
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
        queue_team_=cluster_.invoke("team_get_queue",{{"queueId",current_queue_}});
        if (queue_team_.value("mode","direct")=="team") {copy_team_setup(true);return;}
        const auto queue=std::ranges::find_if(queues_,[&](const Json& row){return row.at("queueId")==current_queue_;});
        if (queue==queues_.end()) throw std::runtime_error("请先选择工作队列");
        std::wstring setup=L"肥鱼重工已启动。请直接使用客户端里的 fatfishbox MCP 工具。";
        if (http_) {
            setup+=L"如果工具列表还没有 fatfishbox，请通过 Streamable HTTP 连接 "+
                widen(http_->url())+L"，无需认证。";
        } else {
            std::wstring executable(32768,L'\0');
            const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
            if (!length || length>=executable.size()) throw std::runtime_error("无法获取肥鱼重工程序路径");
            executable.resize(length);
            setup+=L"本机 HTTP 未启动。可将「"+executable+L"」以 --mcp 参数注册为 stdio 服务。";
        }
        const auto queue_id=widen(current_queue_);
        setup+=L"\n\n当前工作队列："+title(*queue,"name")+L"\nqueueId: "+queue_id+
            L"\n用户指示在这个队列中工作时，派发和后续交接都使用此 queueId。\n\n";
        setup+=widen(fatfish::mcp_handoff_instructions);
        notice_=copy_text(setup)?L"已复制接入说明，粘贴给 Agent 即可":L"剪贴板暂不可用";
        redraw();
    }
    void load_team_settings() {
        team_settings_=cluster_.invoke("team_get_settings");
        for (const auto* key:{"leadPool","developerPool","reviewerPool","integratorPool"}) fields_[std::string("team.")+key]=widen(team_settings_.at(key).get<std::string>());
        for (const auto* key:{"totalConcurrency","developerConcurrency","reviewerConcurrency"}) fields_[std::string("team.")+key]=std::to_wstring(team_settings_.at(key).get<int>());
    }
    void copy_team_setup(bool bind_queue=false) {
        auto setup=fatfish::team_prompt("principal");
        if (bind_queue) setup="当前工作 queueId="+current_queue_+"。先用 team_get_queue 查找绑定记录；已有 project 则按 projectId 续接，无绑定时 team_start_project 必须携带此 queueId（允许接管历史队列）。不要另建队列。用户给出一次任务和授权后，你应启动负责人并持续跟进到最终验收，不再索要下一步指示。\n\n"+setup;
        if (http_) {
            const auto url=http_->url();const auto team_url=url.substr(0,url.size()-4)+"/team";
            setup="肥鱼重工主代理专用 MCP："+team_url+"。请将此地址注册为团队连接；只暴露决策工具，避免携带完整集群工具目录。已有普通 /mcp 连接也能调用 team_*，但工具目录更大。\n\n"+setup;
        } else setup="主代理专用 stdio 入口：FatFishBox.exe --team-mcp。\n\n"+setup;
        notice_=copy_text(widen(setup))?L"已复制详细主代理提示词；粘贴后直接描述任务":L"剪贴板暂不可用";redraw();
    }
    void local_snapshot() {
        ++snapshot_generation_;
        try {
            if (current_queue_.empty()) {
                current_queue_=cluster_.invoke("cluster_desktop_queue").at("queueId").get<std::string>();
                fields_["queueId"]=widen(current_queue_);
            }
            auto snapshot=cluster_.for_queue(current_queue_).desktop_snapshot();
            snapshot["teamQueue"]=cluster_.invoke("team_get_queue",{{"queueId",current_queue_}});
            queues_=snapshot.at("queues");
            if (!session_sync_at_) apply_sessions(snapshot);
            settings_=std::move(snapshot.at("settings"));
            if (team_settings_.empty()) load_team_settings();
            update_pools();
        } catch (const std::exception& e) { fatfish::diagnostics::caught(); notice_ = widen(e.what()); }
    }
    void switch_queue(const std::string& queue) {
        if (current_queue_==queue) return;
        current_queue_=cluster_.invoke("cluster_desktop_queue",{{"queueId",queue}}).at("queueId").get<std::string>();
        fields_["queueId"]=widen(current_queue_);
        queue_progress_expanded_=false;viewport(List::teams).offset=0;
        viewport(List::running).offset=0;
        viewport(List::pending).offset=0;
        viewport(List::processed).offset=0;
        selected_=nullptr;
        clear_result();
        session_sync_at_=0;
        running_=pending_=processed_=Json::array();
        overview_=Json::object();
        local_snapshot();
        if (sessions_worker_.joinable()) refresh_requested_=true;
        else refresh_sessions_automatically();
        redraw();
    }
    void apply_sessions(Json& snapshot) {
        if (snapshot.at("queueId")!=current_queue_) return;
        queue_team_=snapshot.value("teamQueue",Json::object());
        fields_["queueMode"]=widen(queue_team_.value("mode","direct"));
        queues_=std::move(snapshot.at("queues"));
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
        replace(List::processed,processed_,"processed");
        overview_=std::move(snapshot.at("overview"));
        if (page_=="detail" && selected_.is_object()) {
            for (const auto* list:{&running_,&pending_,&processed_}) {
                const auto current=std::ranges::find_if(*list,[&](const Json& row){return row.at("id")==selected_.at("id");});
                if (current==list->end()) continue;
                const bool changed=selected_.at("resultVersion")!=current->at("resultVersion") ||
                    selected_.value("reviewState","")!=current->value("reviewState","");
                selected_=*current;
                if (changed) read_detail_result();
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
        if (page_=="detail" && to!="detail") clear_result();
        ComboBox::dismiss(model_);
        TextBox::cancel_editing(model_,model_.focused);
        model_.ui.editor_focus=none;
        surrogate_=0;
        page_ = std::move(to);
        model_.focused = none;
        model_.hovered = none;
        model_.pressed = none;
        notice_.clear();
        if (page_ == "connection") {load_team_settings();load_connection();}
        redraw();
        if (page_=="sessions") refresh_sessions_automatically();
    }
    void clear_result() {
        ++detail_generation_;
        output_.clear();
        failure_=Json::object();
        detail_read_error_.clear();
        detail_from_history_=false;
        result_loaded_=false;
        result_loading_=false;
    }
    void start_detail_read() {
        if (detail_worker_.joinable()) return;
        const auto scoped=cluster_.for_queue(current_queue_);
        const Json input{{"sessionId",selected_.at("id")}};
        const auto version=selected_.at("resultVersion");
        detail_worker_generation_=detail_generation_;
        detail_worker_=std::jthread([this,scoped,input,version](std::stop_token stop) {
            try {
                auto reply=scoped.invoke("cluster_read_result",input);
                if (reply.at("resultVersion")!=version && !stop.stop_requested()) {
                    auto current=scoped.invoke("cluster_refresh_session",input);
                    if (reply.at("resultVersion")!=current.at("resultVersion"))
                        reply=scoped.invoke("cluster_read_result",input);
                    if (reply.at("resultVersion")!=current.at("resultVersion"))
                        throw std::runtime_error("结果在读取期间发生变化，请重新读取");
                    reply["session"]=std::move(current);
                }
                detail_reply_=std::move(reply);
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                try { detail_worker_error_=widen(error.what()); }
                catch (...) { fatfish::diagnostics::caught(); detail_worker_error_=L"无法读取会话结果"; }
            }
            catch (...) { fatfish::diagnostics::caught(); detail_worker_error_=L"读取结果失败，详情见 logs 日志"; }
            if (!stop.stop_requested()) PostMessageW(window,detail_finished,0,0);
        });
    }
    bool can_read_result() const {
        return selected_.contains("dshId") || !selected_.value("resultVersion","").empty();
    }
    void read_detail_result() {
        clear_result();
        if (can_read_result()) {
            try { start_detail_read(); result_loading_=true; }
            catch (const std::exception& error) { fatfish::diagnostics::caught(); detail_read_error_=widen(error.what()); }
        }
        redraw();
    }
    std::wstring failure_text() const {
        const auto failure=failure_.empty()?selected_.value("failure",Json::object()):failure_;
        const auto kind=title(failure,"kind");
        const auto status=selected_.value("status","");
        const auto error=title(selected_,"error");
        if (failure.empty() && status!="failed" && status!="interrupted") return {};
        static const std::map<std::wstring,std::wstring> descriptions{
            {L"dispatch",L"派发失败"},{L"error",L"DSH 执行报错"},{L"max-tokens",L"达到 Token 上限"},
            {L"blocked",L"执行受阻"},{L"interrupted",L"执行中断"},{L"forked",L"会话发生分支"},
            {L"policy_violation",L"违反执行策略"},{L"tool_arguments_error",L"工具参数错误"}};
        std::wstring result;
        const auto add_line=[&](std::wstring line) {
            if (line.empty()) return;
            if (!result.empty()) result+=L"\r\n";
            result+=line;
        };
        const auto description=descriptions.find(kind);
        if (description!=descriptions.end()) add_line(description->second);
        else if (!kind.empty()) add_line(kind);
        const auto code=title(failure,"code"),stage=title(failure,"stage"),message=title(failure,"message");
        if (!code.empty()) add_line(L"错误代码  "+code);
        if (!stage.empty()) add_line(L"派发阶段  "+stage);
        add_line(message.empty()?error:message);
        for (const auto& reason:failure.value("policyReasons",Json::array()))
            if (reason.is_string()) add_line(L"策略  "+widen(reason.get_ref<const std::string&>()));
        if (result.empty()) return result_loading_?L"正在读取失败原因…":L"DSH 未记录可显示的失败原因";
        return result;
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
    std::vector<Choice> dsh_profile_options() const {
        std::vector<Choice> options;
        for (const auto& [id,profile]:dsh_profiles_.items()) {
            auto name=title(profile,"name");
            if (name.empty()) name=std::filesystem::path(title(profile,"configPath")).stem().wstring();
            if (name.empty()) name=id=="default"?L"原有配置":L"本机授权连接";
            if (id==active_dsh_profile_) name+=L"（当前使用）";
            options.push_back({id,std::move(name)});
        }
        for (std::size_t index=0;index<detected_dsh_.size();++index) {
            const auto& row=detected_dsh_.at(index);
            const auto status=row.value("reachable",false)?L"可连接":row.value("setupRequired",false)?L"待授权":
                row.value("error","")=="未找到本机授权文件"?L"缺少授权":L"无法连接";
            auto name=title(row,"name")+L"（"+status+L"）";
            const auto config=title(row,"configPath");
            if (!config.empty()) name+=L" · "+config;
            options.push_back({"detected:"+std::to_string(index),std::move(name)});
        }
        options.push_back({"new",L"＋ 新建配置"});
        return options;
    }
    void queue_picker(float x,float y,float width,const std::string& surface="root") {
        std::vector<Choice> options;
        for (const auto& queue:queues_) {
            const auto key=queue.at("queueId").get<std::string>();
            options.push_back({key,title(queue,"name")+L" · "+widen(key.substr(0,8))});
        }
        combo("queueId",L"当前队列",x,y,width,std::move(options),surface);
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
        const bool processed=list==List::processed;
        const std::string surface=running?"running":"completed";
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
            running?L"滚动进行中的会话":processed?L"滚动已确认的会话":L"滚动待处理的会话");
        if (running) label(L"进行中",left,y+11,width-125,17,surface,ColorToken::text_primary,true);
        else {
            const auto headers=completed_tabs_.measure_headers(width-125,[](std::wstring_view name){return 15.f*float(name.size());});
            const auto first=model_.ui.widgets.size();
            completed_tabs_.append_headers(model_,surface,"completed.tabs",headers);
            const float top=y+3+title_bar_.height();
            for (std::size_t i=0;i<headers.items.size();++i) {
                const auto bounds=headers.items[i].bounds;
                auto& widget=model_.ui.widgets[first+i];
                widget.area=WidgetArea::absolute;
                widget.bounds={left+bounds.left,top,left+bounds.right,top+bounds.bottom};
            }
            if (!processed && !headers.items.empty() && std::ranges::any_of(rows,[](const Json& row){return row.value("handoffClaimed",false);})) {
                const auto hint=left+headers.items.back().bounds.right+12;
                label(L"1 条已领取",hint,y+15,table_right-125-hint,12,surface,ColorToken::text_secondary);
            }
        }
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
        if (!total) label(running?L"当前没有进行中的会话":processed?L"当前没有已确认的会话":L"当前没有待处理的会话",
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
        const auto queue_width=std::min(350.f,(right-x)*.40f);
        queue_picker(x,57,queue_width);
        combo("queueMode",L"执行方式",x+queue_width+12,57,140,{{"direct",L"直接管理"},{"team",L"团队协作"}});
        button("copy_agent_setup",queue_team_.value("mode","direct")=="team"?L"复制主代理提示词":L"复制接入说明",right-144,81,144,false,!current_queue_.empty());
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
        const auto total=usage.value("today",Json::object());
        const auto input=total.value("uncachedInputTokens",0.0), read=total.value("cacheReadTokens",0.0);
        const auto output=total.value("outputTokens",0.0), write=total.value("cacheWriteTokens",0.0);
        const auto billed=input+read+write;
        const auto hourly=usage.value("hourlyTokens",Json(nullptr));
        const auto amount=[&](double tokens) {
            return usage.value("todaySampled",0)>0 || !usage.value("missing",0)?compact_tokens(tokens):std::wstring(L"—");
        };
        const std::array<std::pair<const wchar_t*,std::wstring>,5> values{{
            {L"今日平均每小时",hourly.is_number()?compact_tokens(hourly.get<double>())+L" Token/h":L"—"},
            {L"今日未缓存输入",amount(input)},{L"今日输出",amount(output)},
            {L"今日缓存读取",amount(read)},{L"今日缓存命中",billed>0?fixed3(read*100/billed)+L"%":L"—"}
        }};
        const bool team_mode=queue_team_.value("mode","direct")=="team";
        panel("usage",{x,121,right,team_mode?185.f:221.f});
        const float usable=right-x-32, first=usable*.29f, cell=(usable-first)/4;
        const std::array<std::pair<const wchar_t*,std::size_t>,2> counts{{
            {L"进行中",running_.size()},{L"已完成",pending_.size()+processed_.size()}
        }};
        const float count_width=usable/4;
        for (std::size_t i=0;!team_mode && i<counts.size();++i) {
            const auto& [name,count]=counts[i];
            const float left=x+16+float(i)*count_width;
            label(name,left,129,count_width-8,12,"usage",ColorToken::text_secondary);
            label(std::to_wstring(count),left,146,count_width-8,15,"usage",ColorToken::text_primary,true);
        }
        for (std::size_t i=0;i<values.size();++i) {
            const float left=x+16+(i?first+float(i-1)*cell:0);
            const float width=(i?cell:first)-8;
            label(values[i].first,left,team_mode?133.f:174.f,width,12,"usage",ColorToken::text_secondary);
            label(values[i].second,left,team_mode?155.f:193.f,width,15,"usage",i?ColorToken::text_primary:ColorToken::accent_primary,true);
        }
        float start=team_mode?198.f:234.f;
        constexpr float gap=12;
        const auto team=queue_team_.value("project",Json());
        if (team.is_object()) {
            const float panel_height=queue_progress_expanded_?std::clamp((h-264)*.45f,96.f,260.f):80.f;
            queue_team_panel(team,x,start,right,start+panel_height);
            start+=panel_height+gap;
        } else if (queue_team_.value("mode","direct")=="team") {
            panel("queue_team_hint",{x,start,right,start+54});
            label(L"已选择团队协作。复制主代理提示词并描述任务，团队将在此队列中启动。",x+16,start+15,right-x-32,13,"queue_team_hint",ColorToken::text_secondary);
            start+=66;
        }
        const auto visible=[&](const Json& rows) {Json out=Json::array();for (const auto& row:rows) if (show_leads_ || row.value("role","")!="lead") out.push_back(row);return out;};
        const auto active_rows=visible(running_),pending_rows=visible(pending_),processed_rows=visible(processed_);
        const float tail=h-30, available=tail-start-gap;
        const float ideal=71+row_height*static_cast<float>(std::clamp(active_rows.size(),std::size_t{1},std::size_t{5}));
        const float active_height=std::min(ideal,std::floor(available*.47f));
        visible_sessions_.clear();
        const bool show_processed=completed_tabs_.selected()=="processed";
        if (available<210) {
            button("sessions.active",L"进行中",x,start,92,!compact_completed_);
            button("sessions.completed",L"已完成",x+100,start,92,compact_completed_);
            if (compact_completed_) session_section(show_processed?List::processed:List::pending,show_processed?processed_rows:pending_rows,x,start+38,right,tail);
            else session_section(List::running,active_rows,x,start+38,right,tail);
        } else {
            session_section(List::running,active_rows,x,start,right,start+active_height);
            session_section(show_processed?List::processed:List::pending,show_processed?processed_rows:pending_rows,x,start+active_height+gap,right,tail);
        }
        if (notice_.empty()) {
            auto summary=overview_.value("codexSources",0)>0?
                L"Codex 登记快照 · "+std::to_wstring(overview_.value("codexSessions",0))+L" 个会话":L"";
            if (overview_.value("unreadableCodexSources",0)>0) summary+=summary.empty()?L"部分 Codex 登记不可读":L"  ·  部分登记不可读";
            if (usage.value("missing",0)>0) {
                if (!summary.empty()) summary+=L"  ·  ";
                summary+=usage_busy_?L"正在同步今日 Token 用量":std::to_wstring(usage.value("missing",0))+L" 个会话今日用量未同步";
            }
            label(std::move(summary),x,h-24,right-x,12,"root",ColorToken::text_secondary);
        }
    }
    void queue_team_panel(const Json& p,float x,float top,float right,float bottom) {
        const auto& progress=p.at("progress");
        panel("queue_team",{x,top,right,bottom});
        const std::map<std::string,std::wstring> states{{"starting",L"启动中"},{"running",L"执行中"},{"paused",L"已暂停"},{"cancelled",L"已取消"},{"completed",L"成果待主代理验收"}};
        const auto state=p.at("status").get<std::string>();
        const auto runtime_errors=p.value("runtimeErrors",Json::object());
        label(L"团队协作 · "+(states.contains(state)?states.at(state):widen(state))+
            (runtime_errors.empty()?L"":L" · 后台异常 "+std::to_wstring(runtime_errors.size())),x+16,top+8,right-x-212,15,"queue_team");
        label(L"领域 "+std::to_wstring(progress.at("completedDomains").get<int>())+L" / "+std::to_wstring(progress.at("totalDomains").get<int>())+
            L"    执行 "+std::to_wstring(progress.at("runningJobs").get<int>())+L"    排队 "+std::to_wstring(progress.at("queuedJobs").get<int>())+
            L"    待决策 "+std::to_wstring(p.at("decisions").size())+
            (p.contains("backlog")?L"    交接余 "+std::to_wstring(p.at("backlog").value("pending",0)):L""),x+16,top+40,right-x-140,12,"queue_team");
        button("team.pause",state=="paused"?L"恢复执行":L"暂停执行",right-192,top+7,88,false,state=="running" || state=="paused","queue_team");
        button("team.expand",queue_progress_expanded_?L"收起进度":L"领域进度",right-96,top+7,80,false,true,"queue_team");
        button("team.leads",show_leads_?L"隐藏负责人":L"显示负责人",right-112,top+41,96,false,true,"queue_team");
        if (!queue_progress_expanded_) return;
        const float body=top+80,content=static_cast<float>(p.at("domains").size())*66+static_cast<float>(p.at("decisions").size())*66+static_cast<float>(runtime_errors.size())*88+8;
        auto& view=viewport(List::teams);view.bounds={x,body+title_bar_.height(),right,bottom+title_bar_.height()};
        view.maximum=std::max(0.f,content-(bottom-body))/row_height;view.offset=std::clamp(view.offset,0.0,view.maximum);
        panel("teams.viewport",{x,body,right-16,bottom},SurfaceVisual::transparent);model_.ui.surfaces.back().scroll=static_cast<float>(view.offset)*row_height;
        if (view.maximum>0) {
            auto& bar=ScrollBar({.label=L"滚动领域进度",.maximum=view.maximum,.value=view.offset,.viewport=(bottom-body)/row_height,.length=bottom-body}).append(model_,"root",view.key);
            bar.area=WidgetArea::absolute;bar.bounds={right-14,view.bounds.top,right,view.bounds.bottom};
        }
        float y=body+2;const auto width=right-x-40;
        for (const auto& failure:runtime_errors) {
            const std::map<std::string,std::wstring> operations{{"create_queue",L"创建队列"},{"refresh_sessions",L"同步会话"},
                {"notify_policy",L"通知负责人"},{"observe_session",L"核对会话"},{"cancel_session",L"取消会话"},
                {"mark_receipt",L"登记回执"},{"backlog_snapshot",L"核对积压"},{"priority_results",L"读取结果"},{"domain_worklist",L"更新领域待办"}};
            const auto operation=failure.value("operation","");
            label(L"后台重试 · "+(operations.contains(operation)?operations.at(operation):widen(operation))+L" · "+local_stamp(failure.value("lastFailureAt",0LL)),x+16,y,width,13,"teams.viewport",ColorToken::status_warning);
            label(title(failure,"message"),x+16,y+25,width,12,"teams.viewport",ColorToken::text_secondary);
            label(L"关联："+title(failure,"scope")+L"    重试时间："+local_stamp(failure.value("retryAt",0LL)),x+16,y+49,width,11,"teams.viewport",ColorToken::text_tertiary);
            y+=88;
        }
        for (const auto& d:p.at("domains")) {
            const std::map<std::string,std::wstring> phases{{"pending",L"等待依赖"},{"working",L"处理中"},{"waiting",L"等待执行结果"},{"decision",L"等待主代理决策"},{"completed",L"已完成"}};
            const auto phase=d.at("state").get<std::string>();
            label(title(d,"name")+L" · "+(phases.contains(phase)?phases.at(phase):widen(phase)),x+16,y,width,14,"teams.viewport");
            label(d.contains("summary")?title(d,"summary"):title(d,"mission"),x+16,y+25,width,12,"teams.viewport",ColorToken::text_secondary);
            y+=66;
        }
        for (const auto& decision:p.at("decisions")) {
            label(L"待决策 · "+title(decision,"question"),x+16,y,width,14,"teams.viewport",ColorToken::status_warning);
            label(L"推荐："+title(decision,"recommendation"),x+16,y+25,width,12,"teams.viewport",ColorToken::text_secondary);y+=66;
        }
    }
    static constexpr float settings_top=64;
    float settings_expander(const std::string& section,std::wstring name,float x,float y,float right,float content_height,
                            const std::function<void(float,const std::string&)>& add_content) {
        const bool expanded=expanded_settings_.value(section,false);
        const auto surface="settings."+section;
        const auto body=y+Expander::header_height;
        const auto bottom=body+(expanded?content_height:0.f);
        panel(surface,{x,y,right,bottom});
        auto& card=model_.ui.surfaces.back();
        card.parent="settings.viewport";
        card.bounds={0,y-settings_top,right-x,bottom-settings_top};
        const auto first=model_.ui.widgets.size();
        auto& header=add(surface+".header",WidgetKind::disclosure,L"",x,y,right-x,Expander::header_height,surface);
        Expander({.title=std::move(name),.expanded=expanded,.width=right-x}).apply(header);
        if (expanded) add_content(body,surface);
        for (std::size_t i=first;i<model_.ui.widgets.size();++i) {
            auto& bounds=model_.ui.widgets[i].bounds;
            bounds.left-=x;
            bounds.right-=x;
            bounds.top-=y+title_bar_.height();
            bounds.bottom-=y+title_bar_.height();
        }
        return bottom+8;
    }
    void connection_page(float x,float right,float h) {
        constexpr float general_height=76,dsh_height=424,mcp_height=272,team_height=351;
        const float width=right-x;
        const float about_width=std::clamp(width*.30f,158.f,310.f);
        const float column_right=right-about_width-14;
        const float viewport_bottom=std::max(settings_top+1,h-29);
        const float viewport_height=viewport_bottom-settings_top;
        const float content_height=4*Expander::header_height+24+
            (expanded_settings_.value("general",false)?general_height:0)+
            (expanded_settings_.value("dsh",false)?dsh_height:0)+
            (expanded_settings_.value("team",false)?team_height:0)+
            (expanded_settings_.value("mcp",false)?mcp_height:0);
        auto& view=viewport(List::settings);
        view.bounds={x,settings_top+title_bar_.height(),column_right,viewport_bottom+title_bar_.height()};
        view.maximum=std::max(0.f,content_height-viewport_height)/row_height;
        view.offset=std::clamp(view.offset,0.0,view.maximum);
        const float settings_right=column_right-(view.maximum>0?16.f:0.f);
        panel("settings.viewport",{x,settings_top,settings_right,viewport_bottom},SurfaceVisual::transparent);
        model_.ui.surfaces.back().scroll=static_cast<float>(view.offset)*row_height;
        if (view.maximum>0) {
            auto& bar=ScrollBar({.label=L"滚动设置",.maximum=view.maximum,.value=view.offset,
                .viewport=viewport_height/row_height,.length=viewport_height}).append(model_,"root",view.key);
            bar.area=WidgetArea::absolute;
            bar.bounds={column_right-14,view.bounds.top,column_right,view.bounds.bottom};
        }
        const float left=x+16, edge=settings_right-16, form=edge-left;
        auto y=settings_expander("general",L"常规",x,settings_top,settings_right,general_height,[&](float body,const std::string& surface) {
            combo("closeBehavior",L"关闭窗口时",left,body+6,form,
                {{"ask",L"每次询问"},{"tray",L"进入托盘"},{"exit",L"直接退出"}},surface);
        });
        y=settings_expander("dsh",L"DSH 连接",x,y,settings_right,dsh_height,[&](float body,const std::string& surface) {
            const float half=(form-14)/2;
            const bool ready=!dsh_draft_needs_authorization();
            combo("dshProfile",L"连接配置",left,body+4,form-108,dsh_profile_options(),surface);
            button("detect_dsh",L"自动识别",edge-100,body+28,100,false,true,surface);
            field("dshName",L"配置名称",left,body+62,form,surface);
            field("configPath",L"DSH 配置文件",left,body+120,form,surface);
            field("authPath",L"本机授权文件（留空时跟随配置）",left,body+178,form,surface);
            combo("permission",L"权限预设",left,body+236,half,preset_options("permissions",L"请选择权限预设"),surface);
            combo("preset",L"Agent 预设",left+half+14,body+236,half,preset_options("agents",L"DSH 默认"),surface);
            button("test_dsh",L"测试连接",left,body+298,84,false,ready,surface);
            button("refresh_presets",L"刷新预设",left+92,body+298,84,false,ready,surface);
            if (std::ranges::any_of(detected_dsh_,[](const Json& row){return row.value("setupRequired",false);}))
                button("authorize_dsh_desktop",L"自动授权",edge-114,body+298,114,true,true,surface);
            label(L"支持的 DSH 版本  "+widen(fatfish::supported_dsh_version),left,body+349,form-128,13,
                surface,ColorToken::text_secondary);
            button("check_dsh_version",L"检测 DSH 版本",edge-120,body+340,120,false,ready,surface);
            button("import",L"导入并启用",left,body+382,100,false,ready,surface);
            button("save_connection",L"保存并启用",edge-104,body+382,104,true,ready,surface);
        });
        y=settings_expander("team",L"团队执行",x,y,settings_right,team_height,[&](float body,const std::string& surface) {
            std::vector<Choice> options{{"",L"请选择已启用的模型"}};
            for (const auto& [key,pool]:settings_.at("pools").items()) if (pool.value("enabled",false))
                options.push_back({key,title(pool,"model")+L" · "+title(pool,"provider")});
            const float half=(form-14)/2;
            combo("team.leadPool",L"项目经理模型",left,body+4,form,options,surface);
            combo("team.developerPool",L"开发与返修模型",left,body+63,form,options,surface);
            combo("team.reviewerPool",L"独立审查模型",left,body+122,form,options,surface);
            field("team.totalConcurrency",L"全团队总并发",left,body+181,half,surface);
            field("team.developerConcurrency",L"开发并发上限",left+half+14,body+181,half,surface);
            field("team.reviewerConcurrency",L"审查并发上限",left,body+240,half,surface);
            label(L"模型用于新项目；并发立即约束后续派发。",left,body+300,form,12,surface,ColorToken::text_secondary);
            button("save_team",L"保存团队方案",left,body+320,116,true,true,surface);
            button("copy_team_setup",L"复制主代理提示词",edge-144,body+320,144,false,true,surface);
        });
        settings_expander("mcp",L"队列与 MCP",x,y,settings_right,mcp_height,[&](float body,const std::string& surface) {
            queue_picker(left,body+4,form,surface);
            field("newQueueName",L"新队列名称",left,body+63,form-116,surface);
            button("create_queue",L"新建队列",edge-100,body+87,100,true,true,surface);
            const bool can_delete=std::ranges::any_of(queues_,[&](const Json& queue){
                return queue.at("queueId")==current_queue_ && !queue.value("legacy",false);
            });
            field("queueName",L"队列名称",left,body+122,form-194,surface);
            button("rename_queue",L"重命名",edge-184,body+146,88,false,!current_queue_.empty(),surface);
            button("delete_queue",L"删除队列",edge-88,body+146,88,false,can_delete,surface);
            label(http_?L"本机 MCP 正在监听":http_error_,left,body+190,form,12,surface,
                http_?ColorToken::status_success:ColorToken::status_error);
            label(http_?widen(http_->url()):L"stdio · FatFishBox.exe --mcp",left,body+210,form,13,surface);
            button("copy_agent_setup",L"复制接入说明",edge-128,body+231,128,true,!current_queue_.empty(),surface);
        });

        const float about_left=right-about_width;
        const float text_width=about_width-36;
        const bool narrow_about=about_width<205.f;
        const float description_height=narrow_about?144.f:84.f;
        panel("about",{about_left,64,right,224+description_height+12});
        if (logo_ && logo_->status()==ImageStatus::ready) {
            auto& image=add("about.logo",WidgetKind::label,L"",about_left+18,84,64,64,"about");
            Image({.source=logo_,.width=64,.height=64,.description=L"肥鱼重工图标"}).apply(image);
        }
        label(L"肥鱼重工",about_left+18,162,text_width,20,"about",ColorToken::text_primary,true);
        label(L"版本 "+widen(FATFISHBOX_VERSION),about_left+18,193,text_width,13,"about",ColorToken::text_secondary);
        auto& description=add("about.description",WidgetKind::label,L"",about_left+18,224,text_width,description_height,"about");
        TextBlock({.text=L"用于管理 DeepSeek Harness 的账号与模型池，按队列派发会话、跟踪结果和返修，并通过本机 MCP 接入 Agent。",
            .font_role=FontRole::body,.font_size=13,.wrap=true,.max_lines=narrow_about?8u:5u,.width=text_width,.height=description_height,
            .foreground=ColorToken::text_secondary}).apply(description);
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
        build_view(float(scale.dips(rect.right)),float(scale.dips(rect.bottom)),IsZoomed(window)!=FALSE);
        InvalidateRect(window,nullptr,FALSE);
    }
    void build_view(float w,float client_height,bool maximized) {
        if (w < 100 || client_height < 100) return;
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
        navigation.settings={"connection",L"设置",L"\uE713",!busy_,{}};
        navigation.expanded_width=w<900?166.f:196.f;
        navigation.pane_toggle_visible=false;
        navigation.search_visible=false;
        navigation_.configure(std::move(navigation));
        navigation_.append(model_,w,client_height,title_bar_.height());
        const float inset=w<900?18.f:26.f;
        const float content=w-navigation_.width()-2*inset;
        const float width=std::min(content,1440.f), x=navigation_.width()+inset+(content-width)/2, right=x+width;
        const std::map<std::string,std::wstring> names{{"sessions",L"会话"},{"pools",L"账号与模型池"},
            {"connection",L"设置"},{"detail",L"会话详情"}};
        label(names.at(page_),x,19,width-246,25);
        if (!notice_.empty()) label(notice_,x,h-24,width,12,"root",ColorToken::text_secondary);
        if (page_=="sessions") {
            session_page(x,right,h);
        } else if (page_=="connection") {
            connection_page(x,right,h);
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
            button("read_row",result_loading_?L"正在读取…":L"重新读取",left+102,174,92,false,can_read_result()&&!result_loading_,"session_detail");
            if (legacy) {
                label(L"来自 Codex · 请在原插件中确认或取消",left+208,181,inner-208,12,"session_detail",ColorToken::text_secondary);
            } else {
                button("cancel_row",L"取消",left+204,174,72,false,linked&&!receipt,"session_detail");
                button("mark_done",L"确认处理",left+286,174,104,true,receipt&&can_read_result()&&!selected_.value("processed",false),"session_detail");
            }
            float result_top=231;
            const auto failure=can_read_result()?failure_text():std::wstring{};
            if (!failure.empty()) {
                const auto height=std::clamp((h-result_top)*.33f,105.f,171.f);
                panel("failure",{x,result_top,right,result_top+height});
                label(L"失败原因",left,result_top+11,inner,17,"failure",ColorToken::status_error,true);
                button("copy_failure",L"复制原因",right-104,result_top+6,88,false,true,"failure");
                auto& box=add("failure_text",WidgetKind::textbox,L"",left,result_top+42,inner,height-52,"failure");
                TextBox(TextBoxOptions{.text=fatfish::result_preview(failure),.read_only=true,.multiline=true,.height=height-52}).apply(box);
                result_top+=height+12;
            }
            panel("result",{x,result_top,right,h-30});
            label(detail_from_history_?L"结果 · DSH 本机历史":L"结果",left,result_top+11,inner,17,"result",ColorToken::text_primary,true);
            if (!output_.empty()) button("copy_result",L"复制结果",right-104,result_top+6,88,false,true,"result");
            const auto text=output_.empty() && !detail_read_error_.empty()?L"结果读取失败\r\n"+detail_read_error_:output_;
            if (!text.empty()) {
                const float height=std::max(35.f,h-result_top-87);
                auto& box=add("output",WidgetKind::textbox,L"",left,result_top+42,inner,height,"result");
                TextBox(TextBoxOptions{.text=fatfish::result_preview(text),.read_only=true,.multiline=true,.height=height}).apply(box);
            } else label(!can_read_result() && selected_.contains("reviewState")?L"对抗审查通过后可查看开发结果":
                result_loading_?L"正在读取结果…":result_loaded_?L"当前结果没有可显示的文本":L"尚无可读取的结果",
                left,result_top+47,inner,13,"result",ColorToken::text_secondary);
        }
        append_widget_layout(layout_,model_);
        if (model_.focused<choice_base) model_.focused=control_id(focus_key);
        if (model_.hovered<choice_base) model_.hovered=control_id(hover_key);
        if (model_.pressed<choice_base) model_.pressed=control_id(pressed_key);
    }
    Json paths() const {
        const auto& auth=fields_.at("authPath");
        const auto& config=fields_.at("configPath");
        const bool automatic=(!config.empty() && auth.empty()) || (automatic_auth_ && auth==detected_auth_);
        return {{"configPath",narrow(config)},{"authPath",narrow(auth)},{"automaticAuth",automatic}};
    }
    Json dsh_profile_input() const {
        auto input=paths();
        input.update(Json{{"revision",settings_.at("revision")},{"profileId",draft_dsh_profile_},
            {"dshName",narrow(fields_.at("dshName"))},{"permission",narrow(fields_.at("permission"))},
            {"preset",narrow(fields_.at("preset"))}});
        return input;
    }
    void test_dsh_connection() {
        job([cluster=cluster_,input=paths()] {
            try { return cluster.invoke("cluster_test_dsh_connection",input); }
            catch (const std::exception& error) { fatfish::diagnostics::caught(); return Json{{"error",error.what()}}; }
        },[this](Json value) {
            const bool failed=value.contains("error");
            if (failed) notice_=title(value,"error");
            else {
                notice_=L"已连接 DSH，实时目录包含 "+std::to_wstring(value.value("accounts",0))+L" 个账号和 "+
                    std::to_wstring(value.value("models",0))+L" 个模型。";
                const auto failures=value.value("failedProviders",0);
                if (failures) notice_+=L"\n"+std::to_wstring(failures)+L" 个提供方的模型目录查询失败，请在 DSH 中检查配置。";
            }
            MessageBoxW(window,notice_.c_str(),failed?L"DSH 连接失败":L"DSH 连接成功",
                MB_OK|(failed?MB_ICONERROR:MB_ICONINFORMATION));
        });
    }
    void check_dsh_version() {
        job([cluster=cluster_,input=paths()] {
            try { return cluster.invoke("cluster_check_dsh_version",input); }
            catch (const std::exception& error) { fatfish::diagnostics::caught(); return Json{{"error",error.what()}}; }
        },[this](Json value) {
            if (value.contains("error")) {
                notice_=title(value,"error");
                MessageBoxW(window,notice_.c_str(),L"无法检测 DSH 版本",MB_OK|MB_ICONWARNING);
                return;
            }
            const bool supported=value.at("supported").get<bool>();
            notice_=L"当前 DSH 版本 "+title(value,"version");
            auto message=notice_+L"\n支持的 DSH 版本 "+widen(fatfish::supported_dsh_version);
            if (!supported) message+=L"\n\n版本不匹配。请将肥鱼重工和 DSH 都升级到最新版，然后重新检测。";
            MessageBoxW(window,message.c_str(),supported?L"DSH 版本匹配":L"DSH 版本不匹配",
                MB_OK|(supported?MB_ICONINFORMATION:MB_ICONWARNING));
        });
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
        catch (const std::exception&) { fatfish::diagnostics::caught();}
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
        } catch (const std::exception& error) { fatfish::diagnostics::caught();
            notice_=L"保存失败 · "+widen(error.what());
            try {settings_=cluster_.invoke("cluster_get_settings");update_pools();}
            catch (const std::exception&) { fatfish::diagnostics::caught();}
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
        if (w->kind==WidgetKind::tab && completed_tabs_.invoke(model_,control)) {
            redraw();
            return;
        }
        if (Expander::is_header(*w)) {
            std::string_view section=w->key;
            section.remove_prefix(std::string_view("settings.").size());
            section.remove_suffix(std::string_view(".header").size());
            const auto key=std::string(section);
            const bool expanding=!expanded_settings_.value(key,false);
            try {
                expanded_settings_=cluster_.invoke("cluster_desktop_settings_expanders",{{"section",key},{"expanded",expanding}});
                if (key=="dsh" && expanding && !busy_ && presets_.empty()) {refresh_presets();return;}
            } catch (const std::exception& error) { fatfish::diagnostics::caught();
                notice_=L"展开状态保存失败 · "+widen(error.what());
            }
            redraw();
            return;
        }
        if (w->visual==WidgetVisual::toggle_switch) {
            save_schedule(w->action,!w->selected);
            return;
        }
        if (w->kind==WidgetKind::select) {
            if (w->key=="queueId") {
                try { queues_=cluster_.invoke("cluster_find_queues").at("queues"); redraw(); }
                catch (const std::exception& error) { fatfish::diagnostics::caught(); notice_=widen(error.what()); redraw(); return; }
                control=control_id("queueId");
            }
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
            if (name=="copy_team_setup") {copy_team_setup();return;}
            if (name=="team.expand") {queue_progress_expanded_=!queue_progress_expanded_;redraw();return;}
            if (name=="team.leads") {show_leads_=!show_leads_;redraw();return;}
            if (name=="sessions.active" || name=="sessions.completed") {compact_completed_=name=="sessions.completed";redraw();return;}
            if (name=="team.pause") {
                const auto p=queue_team_.value("project",Json());if (!p.is_object()) return;
                call("team_control_project",{{"projectId",p.at("projectId")},{"requestId",fatfish::id()},{"action",p.at("status")=="paused"?"resume":"pause"}},[this](Json){queue_team_=cluster_.invoke("team_get_queue",{{"queueId",current_queue_}});});return;
            }
            if (name=="save_team") {
                Json input={{"revision",team_settings_.at("revision")}};
                for (const auto* key:{"leadPool","developerPool","reviewerPool"}) input[key]=narrow(fields_[std::string("team.")+key]);
                for (const auto* key:{"totalConcurrency","developerConcurrency","reviewerConcurrency"}) {
                    const auto value=fields_[std::string("team.")+key];std::size_t end=0;const auto count=std::stoi(value,&end);
                    if (end!=value.size()) throw std::runtime_error("并发须为 1–64 的整数");input[key]=count;
                }
                call("team_update_settings",input,[this](Json value){team_settings_=std::move(value);notice_=L"团队方案已保存，复制提示词后即可开始";});return;
            }
            if (name=="create_queue") {
                call("cluster_create_queue",{{"name",narrow(fields_["newQueueName"])},{"requestId",fatfish::id()}},[this](Json queue) {
                    fields_["newQueueName"].clear();
                    switch_queue(queue.at("queueId").get<std::string>());
                    notice_=L"已创建并切换到新队列";
                });
                return;
            }
            if (name=="delete_queue") {
                const auto queue_id=current_queue_;
                const auto queue=std::ranges::find_if(queues_,[&](const Json& row){return row.at("queueId")==queue_id;});
                if (queue==queues_.end() || queue->value("legacy",false)) return;
                const auto prompt=L"确定删除队列「"+title(*queue,"name")+L"」？\n\n"
                    L"删除后，此队列将从列表中移除，Agent 无法再领取该队列的交接。"
                    L"会话、回执和返修记录仍保存在本机。已派发的 DSH 会话会继续运行。";
                if (MessageBoxW(window,prompt.c_str(),L"确认删除队列",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
                job([this,queue_id] {
                    cluster_.delete_desktop_queue(queue_id);
                    return cluster_.invoke("cluster_desktop_queue");
                },[this](Json selection) {
                    switch_queue(selection.at("queueId").get<std::string>());
                    notice_=L"队列已删除";
                });
                return;
            }
            if (name=="rename_queue") {
                call("cluster_rename_queue",{{"queueId",current_queue_},{"name",narrow(fields_["queueName"])}},[this](Json) {
                    fields_["queueName"].clear();
                    local_snapshot();
                    notice_=L"已重命名当前队列";
                });
                return;
            }
            if (name=="copy_result") {
                notice_=copy_text(output_)?L"已复制结果":L"剪贴板暂不可用";
                redraw();return;
            }
            if (name=="copy_failure") {
                notice_=copy_text(failure_text())?L"已复制失败原因":L"剪贴板暂不可用";
                redraw();return;
            }
            if (name.starts_with("raise.") || name.starts_with("lower.")) {
                const auto id=name.substr(6);
                save_schedule("schedule.concurrency."+id,
                    settings_.at("pools").at(id).at("concurrency").get<int>()+(name.starts_with("raise.")?1:-1));
                return;
            }
            if (name.starts_with("row.")) {
                selected_=visible_sessions_.at(std::stoul(name.substr(4)));
                nav("detail");
                read_detail_result();
                return;
            }
            if (name=="read_row") {read_detail_result();return;}
            if (name=="sync") {
                refresh_sessions_automatically(true);
            } else if (name=="detect_dsh") {
                detect_dsh_profiles();
            } else if (name=="authorize_dsh_desktop") {
                authorize_desktop();
            } else if (name=="test_dsh") {
                test_dsh_connection();
            } else if (name=="check_dsh_version") {
                check_dsh_version();
            } else if (name=="import") {
                call("cluster_import_models",page_=="connection"?dsh_profile_input():Json::object(),[this](Json value) {
                    std::erase_if(fields_,[](const auto& entry){return entry.first.starts_with("schedule.");});
                    local_snapshot();nav("pools");notice_=L"已导入 "+std::to_wstring(value.value("accounts",0))+L" 个账号、"+std::to_wstring(value.value("models",0))+L" 个模型";
                    if (const auto configured=value.value("configuredModels",0);configured)
                        notice_+=L" · "+std::to_wstring(configured)+L" 个模型等待 DSH 确认可用";
                    if (const auto failures=value.value("failedProviders",0);failures)
                        notice_+=L" · "+std::to_wstring(failures)+L" 个提供方目录查询失败";
                    if (value.value("configurationUnavailable",false)) notice_+=L" · 未能读取 DSH 模型配置";
                });
            } else if (name=="save_connection") {
                call("cluster_save_dsh_profile",dsh_profile_input(),[this](Json value) {
                    settings_=std::move(value);
                    local_snapshot();
                    load_connection(L"已保存并启用所选 DSH 连接");
                });
            } else if (name.ends_with("_row") || name.starts_with("mark_")) {
                const std::map<std::string,std::string> operations{{"refresh_row","cluster_refresh_session"},{"cancel_row","cluster_cancel_session"},
                    {"mark_done","cluster_mark_receipt"}};
                Json input{{"sessionId",selected_.at("id")}};
                if (name.starts_with("mark_")) input.update(Json{{"resultVersion",selected_.at("resultVersion")},{"processed",name=="mark_done"}});
                call(operations.at(name),input,[this,name](Json value) {
                    const bool changed=value.at("resultVersion")!=selected_.at("resultVersion") || value.at("status")!=selected_.at("status");
                    selected_=std::move(value);local_snapshot();
                    if (changed || name=="refresh_row") read_detail_result();
                });
            }
        } catch (const std::exception& error) { fatfish::diagnostics::caught();notice_=widen(error.what());redraw();}
    }
};

LRESULT dispatch_window_impl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (msg==WM_NCCREATE) {
        app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->window=hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd,msg,wp,lp);
    if (app->taskbar_recreated(msg)) return 0;
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
    case WM_RBUTTONUP: app->session_context(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
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
    case detail_finished: app->receive_detail_result();return 0;
    case WM_TIMER:
        if (wp==usage_timer) { app->sync_usage();return 0; }
        if (wp==sessions_timer) { app->refresh_sessions_automatically();return 0; }
        break;
    case tray_notification: app->tray_event(wp,lp);return 0;
    case WM_CLOSE: app->closing();return 0;
    case WM_DESTROY: app->remove_tray_icon();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

LRESULT CALLBACK dispatch_window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    try { return dispatch_window_impl(hwnd,msg,wp,lp); }
    catch (...) {
        fatfish::diagnostics::caught();
        if (msg==WM_PAINT) ValidateRect(hwnd,nullptr);
        if (msg==WM_CREATE) return -1;
        if (msg==WM_NCCREATE) return FALSE;
        // Avoid recursive error dialogs from paint/timer failures.
        static DWORD last_notice=0;
        const DWORD tick=GetTickCount();
        if (!last_notice || tick-last_notice>30000) {
            last_notice=tick;
            MessageBoxW(hwnd,L"操作发生异常，已记录到程序旁的 logs 文件夹。请重试；若持续出现，请重启程序并查看日志。",L"肥鱼重工 · 操作异常",MB_OK|MB_ICONERROR);
        }
        return 0;
    }
}

int render_offscreen(std::vector<std::wstring> arguments) {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=0;
    try {
        if (arguments.size()!=2 && arguments.size()!=5)
            throw std::runtime_error("用法：--render-offscreen <settings|sessions|pools> <output.png> [width height dpi]");
        const auto page=narrow(arguments[0]);
        if (page!="settings" && page!="sessions" && page!="pools") throw std::runtime_error("未知离屏页面");
        const auto number=[&](std::size_t index,UINT maximum) {
            const auto& value=arguments[index];
            std::size_t end{};
            const auto parsed=std::stoul(value,&end);
            if (end!=value.size() || parsed==0 || parsed>maximum) throw std::runtime_error("离屏尺寸或 DPI 无效");
            return static_cast<UINT>(parsed);
        };
        const auto width=arguments.size()==5?number(2,8192):1100;
        const auto height=arguments.size()==5?number(3,8192):720;
        const auto dpi=arguments.size()==5?number(4,768):96;
        if (width*96<minimum_width*dpi || height*96<minimum_height*dpi)
            throw std::runtime_error("离屏尺寸小于窗口最小尺寸");
        if (FAILED(initialized)) throw std::runtime_error("离屏渲染 COM 初始化失败");
        App app;
        app.render_offscreen(page,arguments[1],width,height,dpi);
    } catch (const std::exception& error) { fatfish::diagnostics::caught();
        fatfish::mcp_diagnostic(error.what());
        result=1;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
}

int WINAPI application_main(HINSTANCE instance, int show) {
    int argc{};
    if (auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc)) {
        const std::wstring mode=argc>=2?argv[1]:L"";
        if (mode==L"--render-offscreen") {
            std::vector<std::wstring> arguments(argv+2,argv+argc);
            LocalFree(argv);
            return render_offscreen(std::move(arguments));
        }
        if (mode==L"--team-call" && argc==4) {
            const fatfish::fs::path request=argv[2],response=argv[3];LocalFree(argv);
            try {
                const auto input=fatfish::read_json(request);
                const auto name=input.at("tool").get<std::string>();
                if (!name.starts_with("team_")) throw std::runtime_error("此入口只允许 team_* 工具");
                const auto result=Cluster().invoke_mcp(name,input.value("arguments",Json::object()));
                std::ofstream output(response,std::ios::binary);output<<Json{{"ok",true},{"result",result}}.dump();output.close();return output?0:1;
            } catch (const fatfish::DispatchError& e) {
                std::ofstream output(response,std::ios::binary);output<<Json{{"ok",false},{"error",e.what()},{"details",e.details}}.dump();return 1;
            } catch (const std::exception& e) {
                std::ofstream output(response,std::ios::binary);output<<Json{{"ok",false},{"error",e.what()}}.dump();return 1;
            }
        }
        LocalFree(argv);
        if (argc==2 && mode==L"--mcp") return fatfish::run_mcp_stdio();
        if (argc==2 && mode==L"--team-mcp") return fatfish::run_mcp_stdio(true);
        if (argc==2 && mode==L"--mcp-http") return fatfish::run_mcp_http();
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
        BOOL status{};
        while ((status=GetMessageW(&message,nullptr,0,0))>0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (status==-1) throw std::runtime_error("Windows 消息读取失败");
    } catch (const std::exception& error) { fatfish::diagnostics::caught();
        MessageBoxW(nullptr,widen(error.what()).c_str(),L"肥鱼重工",MB_ICONERROR);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    fatfish::diagnostics::initialize();
    try { return application_main(instance,show); }
    catch (...) {
        fatfish::diagnostics::caught();
        MessageBoxW(nullptr,L"程序启动失败，详细信息已记录到程序旁的 logs 文件夹。",L"肥鱼重工",MB_OK|MB_ICONERROR);
        return 1;
    }
}
