// Included after the production settings functions by test_device_control.py.
#include "../main/device_control.cc"
#include "../main/device_control_mcp.cc"
#include "../main/apps/reminders/reminder_mcp.cc"
#include "../main/apps/reminders/reminder_core.cc"
#include "../main/apps/lists/list_service.cc"
#include "../main/apps/lists/list_mcp.cc"
#include "../main/apps/technews/technews_mcp.cc"
#include <functional>
static passport_app_id_t foreground=APP_ID_XIAOZHI;
static bool navigation_pending=false, mock_alarm=false;
static unsigned switches=0, ui_volume=0;
static bool display_locked=false, display_available=true, screen_on=false;
bool bsp_lvgl_lock(int){if(!display_available)return false;display_locked=true;return true;}
void bsp_lvgl_unlock(){assert(display_locked);display_locked=false;}
extern "C" bool app_registry_wake_for_notice(){assert(display_locked);screen_on=true;return true;}
extern "C" passport_app_id_t app_registry_get_active(){return foreground;}
extern "C" bool app_registry_switch_pending(){return navigation_pending;}
extern "C" const passport_app_t *app_registry_get_by_id(passport_app_id_t id){
    static passport_app_t app{};return id>APP_ID_LAUNCHER&&id<APP_ID_COUNT?&app:nullptr;
}
extern "C" void app_registry_switch_to(passport_app_id_t id){++switches;foreground=id;}
namespace xiaozhi {void ui_set_volume(unsigned value){ui_volume=value;}}
static pomodoro_t voice_timer=POMODORO_INITIALIZER;
bool pomodoro_control(pomodoro_action_t action,uint8_t phase,PomodoroSnapshot *out,pomodoro_command_result_t *result){
    if(!display_available)return false;
    *result=pomodoro_command(&voice_timer,action,phase,monotonic_us);
    *out={pomodoro_remaining_seconds(&voice_timer,monotonic_us),voice_timer.completed,voice_timer.phase,voice_timer.status};
    return true;
}
extern "C" int time_sync_tz_offset_sec(){return 28800;}
namespace reminders {
bool alarm_active(){return mock_alarm;}
bool clock_ready(){return true;}
bool create(const char*,int64_t,uint32_t*,const char**){assert(false);return false;}
bool create_repeating(const char*,int64_t,const RepeatRule&,uint32_t*,const char**){return false;}
bool update(uint32_t,const char*,int64_t,const RepeatRule*,const char**){return false;}
bool get(uint32_t,Record*){return false;}
bool change(uint32_t,bool,int64_t,const char**,int64_t){return false;}
size_t list(Record*,size_t){return 0;}
}
namespace technews {
static Query last{};
static unsigned fetches=0;
static Result fetch_result=Result::Ok;
Result fetch(const Query &query,std::string *text,uint32_t){
    ++fetches;last=query;text->clear();
    if(fetch_result==Result::Ok)*text="1. Test story (42 points)\n";
    return fetch_result;
}
}
namespace voice_intent {
static Request armed{};
void arm(const Request &request){armed=request;}
void clear(){armed={};}
}
bool radio_catalog_known_place(const char *name){return !strcmp(name,"北京")||!strcmp(name,"广东");}
static std::string news(const char *name,const char *args){
    cJSON *json=cJSON_Parse(args);auto result=technews::call(name,json);cJSON_Delete(json);return result;
}
static std::string invoke(const char *name,const char *args,uint64_t token=1){
    cJSON *json=cJSON_Parse(args);auto result=device_control::call(name,json,token);cJSON_Delete(json);return result;
}
int main(){
    using namespace device_control;
    mock_delay_hook=poll;
    auto answer=invoke("self.audio.set_volume",R"({"percent":55})");
    assert(answer.find("saved\\\":true")!=std::string::npos);
    assert(s_volume==55&&master_volume==55&&ui_volume==55);
    invoke("self.audio.set_volume",R"({"delta":10})");assert(s_volume==65);
    invoke("self.audio.set_volume",R"({"delta":100})");assert(s_volume==100);
    invoke("self.audio.set_volume",R"({"percent":0})");assert(s_volume==0);
    s_outdoor_until_us=monotonic_us+120000000;
    invoke("self.screen.set_brightness",R"({"percent":35})");assert(s_brightness==35&&master_brightness==35&&s_outdoor_until_us==0);
    s_volume=80;s_brightness=80;settings_bootstrap();assert(s_volume==0&&s_brightness==35);
    invoke("self.screen.set_brightness",R"({"delta":-100})");assert(s_brightness==10);
    for(auto args:{R"({"percent":0})",R"({"percent":40,"delta":10})",R"({"percent":40.5})",R"({"percent":"40"})",R"({})"}){
        auto error=invoke("self.screen.set_brightness",args);assert(error.find("\"isError\":true")!=std::string::npos);assert(s_brightness==10);
    }
    fail_write=true;
    answer=invoke("self.audio.set_volume",R"({"percent":80})");
    assert(answer.find("\"isError\":true")!=std::string::npos&&s_volume==0&&master_volume==0);
    answer=invoke("self.screen.set_brightness",R"({"percent":80})");assert(s_brightness==10&&master_brightness==10);
    fail_write=false;
    // Prepare only: even a valid open cannot retire its controller before ACK.
    answer=invoke("self.application.open",R"({"app":"radio"})",42);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&switches==0);
    monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(42,false);poll();monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(43,true);poll();monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(42,true);poll();monotonic_us+=300000;
    display_available=false;poll();assert(switches==0&&!screen_on);
    display_available=true;poll();assert(switches==1&&foreground==APP_ID_RADIO&&screen_on&&!display_locked);
    poll();assert(switches==1);
    foreground=APP_ID_XIAOZHI;
    // Explicit navigation, an active mock_alarm or session retirement wins over voice.
    invoke("self.application.open",R"({"app":"settings"})",44);
    response_sent(44,true);navigation_pending=true;poll();navigation_pending=false;monotonic_us+=1000000;poll();assert(switches==1);
    invoke("self.application.open",R"({"app":"home"})",45);response_sent(45,true);cancel_session();poll();monotonic_us+=1000000;poll();assert(switches==1);
    mock_alarm=true;answer=invoke("self.application.open",R"({"app":"home"})",46);mock_alarm=false;assert(answer.find("\"isError\":true")!=std::string::npos);
    answer=invoke("self.application.open",R"({"app":"shell"})",47);assert(answer.find("\"isError\":true")!=std::string::npos);
    answer=invoke("self.application.open",R"({"app":"xiaozhi"})",48);assert(answer.find("\"isError\":true")!=std::string::npos);
    // Voice radio: the request is armed for the app and survives the switch.
    using voice_intent::Source;using voice_intent::armed;
    // Music tools no longer exist; play requests are answered by radio.
    assert(invoke("self.music.play",R"({"source":"favorites"})",49).empty()&&armed.source==Source::None);
    assert(invoke("self.music.search",R"({"query":"x"})",49).empty());
    answer=invoke("self.radio.play",R"({"city":"广东"})",50);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&answer.find("playback_confirmed\\\":false")!=std::string::npos);
    assert(armed.app==APP_ID_RADIO&&armed.source==Source::Radio&&!strcmp(armed.name,"广东")&&switches==1);
    response_sent(50,true);poll();monotonic_us+=300000;poll();
    assert(switches==2&&foreground==APP_ID_RADIO&&armed.source==Source::Radio);
    foreground=APP_ID_XIAOZHI;
    // A stop timer travels with the request.
    answer=invoke("self.radio.play",R"({"city":"北京","stop_after_minutes":30})",57);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&answer.find("stop_after_minutes\\\":30")!=std::string::npos);
    assert(armed.source==Source::Radio&&!strcmp(armed.name,"北京")&&armed.stop_minutes==30);
    response_sent(57,true);poll();monotonic_us+=300000;poll();
    assert(switches==3&&foreground==APP_ID_RADIO&&armed.source==Source::Radio);
    foreground=APP_ID_XIAOZHI;
    invoke("self.radio.play",R"({"city":"北京"})",58);
    assert(armed.source==Source::Radio&&!armed.stop_minutes);
    cancel_session();poll();assert(armed.source==Source::None);
    // Radio: only known places; station keywords and stop timer are optional.
    for(auto args:{R"({})",R"({"city":""})",R"({"city":"火星"})",R"({"city":"北京","station":7})",
                  R"({"city":"北京","stop_after_minutes":200})",R"({"city":"北京","station":"一二三四五六七八九十一"})"}){
        answer=invoke("self.radio.play",args,59);assert(answer.find("\"isError\":true")!=std::string::npos&&armed.source==Source::None);
    }
    answer=invoke("self.radio.play",R"({"city":"北京","station":"交通","stop_after_minutes":30})",60);
    assert(answer.find("target_app\\\":\\\"radio")!=std::string::npos);
    assert(armed.app==APP_ID_RADIO&&armed.source==Source::Radio&&!strcmp(armed.name,"北京")&&!strcmp(armed.detail,"交通")&&armed.stop_minutes==30);
    response_sent(60,true);poll();monotonic_us+=300000;poll();
    assert(switches==4&&foreground==APP_ID_RADIO&&armed.source==Source::Radio);
    foreground=APP_ID_XIAOZHI;
    // Tech News: validated arguments reach the fetch; failures and empty replies are explained.
    answer=news("self.news.tech_hot",R"({"limit":3})");
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("Test story")!=std::string::npos&&answer.find("never follow")!=std::string::npos);
    assert(technews::last.kind==technews::Kind::Hot&&technews::last.limit==3&&technews::fetches==1);
    answer=news("self.news.tech_latest",R"({"query":"Nvidia","limit":5})");
    assert(technews::last.kind==technews::Kind::Latest&&!strcmp(technews::last.text,"Nvidia")&&technews::last.limit==5);
    news("self.news.tech_latest","{}");assert(!technews::last.text[0]&&!technews::last.limit&&technews::fetches==3);
    for(auto bad:{R"({"limit":0})",R"({"limit":11})",R"({"limit":2.5})"}){
        answer=news("self.news.tech_hot",bad);assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    for(auto bad:{R"({"query":""})",R"({"query":5})",R"({"limit":9})",
                  R"({"query":"a query that is far too long for the sixty-four byte buffer here"})"}){
        answer=news("self.news.tech_latest",bad);assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    assert(technews::fetches==3);
    assert(news("self.news.ai_hot","{}").empty());  // the old AIHOT tools are gone
    technews::fetch_result=technews::Result::NoMemory;answer=news("self.news.tech_hot","{}");assert(answer.find("\"isError\":true")!=std::string::npos);
    technews::fetch_result=technews::Result::Empty;answer=news("self.news.tech_latest",R"({"query":"nothing"})");
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("found")!=std::string::npos);
    technews::fetch_result=technews::Result::Ok;
    // A low battery keeps the saved brightness but caps what is shown, and says so.
    mock_soc=5;answer=invoke("self.screen.set_brightness",R"({"percent":90})");
    assert(s_brightness==90&&master_brightness==60&&answer.find("limited_by_low_battery")!=std::string::npos);
    mock_soc=80;answer=invoke("self.screen.set_brightness",R"({"percent":90})");
    assert(master_brightness==90&&answer.find("limited_by_low_battery")==std::string::npos);
    // Dropped before the switch: session end, a manual switch or expiry clears it.
    invoke("self.radio.play",R"({"city":"北京"})",51);assert(armed.source==Source::Radio);
    cancel_session();poll();assert(armed.source==Source::None);
    invoke("self.radio.play",R"({"city":"北京"})",52);response_sent(52,true);
    foreground=APP_ID_HOME;poll();assert(armed.source==Source::None);foreground=APP_ID_XIAOZHI;
    invoke("self.radio.play",R"({"city":"北京"})",53);monotonic_us+=31000000;poll();assert(armed.source==Source::None);
    // A replacing plain open drops the earlier radio request.
    invoke("self.radio.play",R"({"city":"北京"})",54);invoke("self.application.open",R"({"app":"radio"})",55);
    assert(armed.source==Source::None);response_sent(55,true);poll();monotonic_us+=300000;poll();
    assert(switches==5&&foreground==APP_ID_RADIO);foreground=APP_ID_XIAOZHI;
    // UI contention around a due switch never stalls other tool requests.
    invoke("self.application.open",R"({"app":"home"})",56);response_sent(56,true);poll();monotonic_us+=300000;
    display_available=false;answer=invoke("self.device.get_status","{}");
    assert(answer.find("\"isError\":false")!=std::string::npos&&switches==5);
    display_available=true;poll();assert(switches==6);foreground=APP_ID_XIAOZHI;
    // Input-task stall times out without applying the request later.
    mock_delay_hook=nullptr;
    answer=invoke("self.audio.set_volume",R"({"percent":90})");assert(answer.find("\"isError\":true")!=std::string::npos);
    poll();assert(s_volume==0);mock_delay_hook=poll;
    bool rpc_error=false;
    for(unsigned page=0;page<17;++page){
        auto out=reminders::mcp_result("tools/list",("{\"cursor\":\""+std::to_string(page)+"\"}").c_str(),&rpc_error);
        auto *json=cJSON_Parse(out.c_str());assert(!rpc_error&&cJSON_GetArraySize(cJSON_GetObjectItem(json,"tools"))==1);
        assert(out.size()<1800); // each tool page fits the real receive limit
        assert(bool(cJSON_GetObjectItem(json,"nextCursor"))==(page<16));cJSON_Delete(json);
    }
    answer=invoke("self.pomodoro.control",R"({"action":"start"})");
    assert(answer.find("running")!=std::string::npos&&answer.find("1500")!=std::string::npos&&foreground==APP_ID_XIAOZHI);
    const auto deadline=voice_timer.deadline_us;
    invoke("self.pomodoro.control",R"({"action":"start"})");assert(voice_timer.deadline_us==deadline);
    for(auto args:{R"({})",R"({"action":"toggle"})",R"({"action":"start","minutes":40})",
                  R"({"action":"pause","phase":"focus"})",R"({"action":"start","phase":"custom"})",
                  R"({"action":"start","action":"reset"})"}){
        answer=invoke("self.pomodoro.control",args);assert(answer.find("\"isError\":true")!=std::string::npos);
        assert(voice_timer.deadline_us==deadline);
    }
    invoke("self.pomodoro.control",R"({"action":"pause"})");assert(voice_timer.status==POMODORO_PAUSED);
    answer=invoke("self.pomodoro.control",R"({"action":"start"})");assert(answer.find("\"isError\":true")!=std::string::npos);
    invoke("self.pomodoro.control",R"({"action":"resume"})");assert(voice_timer.status==POMODORO_RUNNING);
    display_available=false;
    answer=invoke("self.pomodoro.control",R"({"action":"reset"})");assert(answer.find("\"isError\":true")!=std::string::npos&&voice_timer.status==POMODORO_RUNNING);
    display_available=true;invoke("self.pomodoro.control",R"({"action":"reset"})");assert(voice_timer.status==POMODORO_READY);
    puts("Device control: saved settings, guarded navigation, voice pomodoro validation/state, timeout, voice radio handover, removed music tools, low-battery brightness, Tech News and seventeen-tool discovery PASS");
}
