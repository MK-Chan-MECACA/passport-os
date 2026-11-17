#include "device_control.h"
#include "apps/radio/radio_catalog.h"
#include <apps/xiaozhi/xiaozhi_ui.h>
#include <cJSON.h>
#include <cstring>
#include <memory>

namespace device_control {
namespace {
const char *const schemas[] = {
R"({"name":"self.pomodoro.control","description":"Control or query the device's actual pomodoro timer. action: status/start/pause/resume/reset. start defaults to focus=25 minutes; short_break=5 and long_break=15 are the only other durations. Do not claim unsupported custom durations. phase is allowed only with start. Starting an already running same phase does not restart it; use resume for a paused timer. Reset only on explicit user request; never automatically reset to resolve a conflict. Starting stays in Xiaozhi and continues counting after leaving it or screen sleep. Open the pomodoro app separately only if asked. Read actual returned status and remaining_seconds. Timer is not saved across power-off. Completion shows on screen; sound waits while another audio app owns the speaker.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["status","start","pause","resume","reset"]},"phase":{"type":"string","enum":["focus","short_break","long_break"]}},"required":["action"],"additionalProperties":false}})",
R"({"name":"self.device.get_status","description":"Read this device's saved speaker volume, screen brightness and current app. Volume and brightness are percentages. Use the actual tool result; do not guess device state.","inputSchema":{"type":"object","properties":{}}})",
R"({"name":"self.audio.set_volume","description":"Change AND persist this device's master speaker volume. Use exactly one of percent (absolute 0..100) or delta (signed percentage points -100..100). For louder/quieter without an amount use delta +10/-10. Zero mutes speech and media; timer and reminder alarms still ring at their fixed alert level. Only claim saved on saved=true; report the returned volume_percent. No external player or server setting is needed.","inputSchema":{"type":"object","properties":{"percent":{"type":"integer","minimum":0,"maximum":100},"delta":{"type":"integer","minimum":-100,"maximum":100}}}})",
R"({"name":"self.screen.set_brightness","description":"Change AND persist this device's screen brightness. Use exactly one of percent (absolute 10..100) or delta (signed percentage points -100..100). For brighter/dimmer without an amount use delta +10/-10. Values clamp to 10..100 to keep the display readable. Only claim saved on saved=true; report brightness_percent. Survives reboot and screen sleep/wake.","inputSchema":{"type":"object","properties":{"percent":{"type":"integer","minimum":10,"maximum":100},"delta":{"type":"integer","minimum":-100,"maximum":100}}}})",
R"({"name":"self.application.open","description":"Open a built-in device app on explicit request. radio=Radio; reminders=Reminders; lists=Lists; pomodoro=Pomodoro; settings=Settings; home=Home; launcher=app list; stopwatch=Stopwatch; tetris=Tetris; sound_jump=Sound Jump; game=Why Barking voice game; muse=Muse; voice=AI Voice; xiaozhi=Xiaozhi. Ask which app if ambiguous. Opens its screen only. Switching exits this Xiaozhi conversation after the tool reply; do not promise continued background voice control.","inputSchema":{"type":"object","properties":{"app":{"type":"string","enum":["radio","reminders","lists","pomodoro","settings","home","launcher","stopwatch","tetris","sound_jump","game","muse","voice","xiaozhi"]}},"required":["app"]}})",
R"({"name":"self.radio.play","description":"Play live internet radio for a Malaysian city, town or state in this device's Radio app, e.g. play radio in Penang = city Penang; use the English place name. station: optional keywords of a station name such as BFM or Traxx; otherwise the most popular local station plays. stop_after_minutes pauses the radio later, e.g. stop in half an hour = 30. This device has no music library: for any request to play music or a song, offer live radio with this tool instead. Exits this Xiaozhi conversation after the reply. Stations come from a public directory, so do not promise a particular one; the result shows on screen.","inputSchema":{"type":"object","properties":{"city":{"type":"string","maxLength":40},"station":{"type":"string","maxLength":10},"stop_after_minutes":{"type":"integer","minimum":1,"maximum":180}},"required":["city"]}})"
};
struct App {const char *key; passport_app_id_t id;};
constexpr App apps[] = {
    {"radio",APP_ID_RADIO},{"reminders",APP_ID_REMINDERS},
    {"lists",APP_ID_LISTS},{"pomodoro",APP_ID_POMODORO},{"settings",APP_ID_SETTINGS},{"home",APP_ID_HOME},
    {"launcher",APP_ID_LAUNCHER},{"stopwatch",APP_ID_STOPWATCH},{"tetris",APP_ID_TETRIS},
    {"sound_jump",APP_ID_SOUND_JUMP},{"game",APP_ID_GAME},{"muse",APP_ID_MUSE},
    {"voice",APP_ID_VOICE},{"xiaozhi",APP_ID_XIAOZHI}
};
const char *app_key(passport_app_id_t id) {
    for (const auto &app : apps) if (app.id == id) return app.key;
    return "unknown";
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string print(cJSON *value) {
    char *raw=cJSON_PrintUnformatted(value);
    std::string text=raw?raw:"{}";cJSON_free(raw);return text;
}
std::string reply(cJSON *value, bool error) {
    Json result(cJSON_CreateObject(),cJSON_Delete);
    cJSON *content=cJSON_AddArrayToObject(result.get(),"content");
    cJSON *item=cJSON_CreateObject();cJSON_AddItemToArray(content,item);
    cJSON_AddStringToObject(item,"type","text");
    cJSON_AddStringToObject(item,"text",print(value).c_str());
    cJSON_AddBoolToObject(result.get(),"isError",error);return print(result.get());
}
std::string fail(const char *reason) {
    Json value(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddBoolToObject(value.get(),"saved",false);
    cJSON_AddStringToObject(value.get(),"error",reason);return reply(value.get(),true);
}
bool integer(const cJSON *value,int low,int high) {
    return cJSON_IsNumber(value) && value->valuedouble>=low && value->valuedouble<=high &&
        value->valuedouble==value->valueint;
}
const char *const timer_actions[] = {"status","start","pause","resume","reset"};
const char *const timer_phases[] = {"focus","short_break","long_break"};
const char *const timer_states[] = {"ready","running","paused","finished"};
// Copies a text argument; error names what is missing in words the user hears.
bool text_field(const cJSON *args,const char *key,bool required,char *out,size_t capacity,const char **error) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(args,key);
    const bool city=!std::strcmp(key,"city");
    if (!value && !required) return true;
    if (!cJSON_IsString(value) || !value->valuestring[0]) {
        *error=city?"Name the city or state to play":"Station name must be text";
        return false;
    }
    if (std::strlen(value->valuestring)>=capacity) {
        *error=city?"City name too long":"Station name too long";
        return false;
    }
    std::strcpy(out,value->valuestring);
    return true;
}
}
size_t tool_count(){return sizeof(schemas)/sizeof(schemas[0]);}
const char *tool_schema(size_t index){return index<tool_count()?schemas[index]:nullptr;}
std::string call(const char *name,const cJSON *args,uint64_t token) {
    Request request;
    if (!std::strcmp(name,"self.device.get_status")) request.action=Action::Status;
    else if (!std::strcmp(name,"self.audio.set_volume")) request.action=Action::Volume;
    else if (!std::strcmp(name,"self.screen.set_brightness")) request.action=Action::Brightness;
    else if (!std::strcmp(name,"self.application.open")) request.action=Action::Open;
    else if (!std::strcmp(name,"self.pomodoro.control")) request.action=Action::Pomodoro;
    else if (!std::strcmp(name,"self.radio.play")) request.action=Action::Handover;
    else return {};
    if (!cJSON_IsObject(args)) return fail("Provide valid device control arguments");
    request.token=token;
    if (request.action==Action::Volume || request.action==Action::Brightness) {
        const cJSON *percent=cJSON_GetObjectItemCaseSensitive(args,"percent");
        const cJSON *delta=cJSON_GetObjectItemCaseSensitive(args,"delta");
        if (bool(percent)==bool(delta)) return fail("Give exactly one of percent or delta");
        const cJSON *value=percent?percent:delta;
        if (!integer(value,delta?-100:request.action==Action::Volume?0:10,100))
            return fail("Volume is 0 to 100, brightness 10 to 100; delta is a whole percentage");
        request.value=value->valueint;request.relative=delta!=nullptr;
    } else if (request.action==Action::Pomodoro) {
        unsigned fields=0;
        for (const cJSON *field=args->child;field;field=field->next) {
            const unsigned bit=field->string&&!std::strcmp(field->string,"action")?1:
                field->string&&!std::strcmp(field->string,"phase")?2:0;
            if (!bit || (fields&bit)) return fail("Pomodoro accepts only action and phase; custom durations are not supported");
            fields|=bit;
        }
        const auto *action=cJSON_GetObjectItemCaseSensitive(args,"action");
        const auto *phase=cJSON_GetObjectItemCaseSensitive(args,"phase");
        if (!cJSON_IsString(action)) return fail("Say whether to check, start, pause, resume or reset the pomodoro");
        unsigned index=0;
        while (index<5 && std::strcmp(action->valuestring,timer_actions[index])) ++index;
        if (index==5) return fail("Unsupported pomodoro action");
        request.timer_action=static_cast<pomodoro_action_t>(index);
        if (phase) {
            if (request.timer_action!=POMODORO_START || !cJSON_IsString(phase))
                return fail("Focus or break type can only be given with start");
            index=0;
            while (index<3 && std::strcmp(phase->valuestring,timer_phases[index])) ++index;
            if (index==3) return fail("Only 25-minute focus, 5-minute short break and 15-minute long break are supported");
            request.timer_phase=static_cast<uint8_t>(index);
        }
    } else if (request.action==Action::Open) {
        const auto *app=cJSON_GetObjectItemCaseSensitive(args,"app");
        if (!cJSON_IsString(app)) return fail("Say which app to open");
        for (const auto &candidate:apps) if (!std::strcmp(app->valuestring,candidate.key)) request.app=candidate.id;
        if (request.app==APP_ID_COUNT) return fail("No such app on this device; choose a built-in app");
    } else if (request.action==Action::Handover) {
        using voice_intent::Source;
        auto &media=request.handover;
        const char *error=nullptr;
        const auto *stop=cJSON_GetObjectItemCaseSensitive(args,"stop_after_minutes");
        if (stop) {
            if (!integer(stop,1,voice_intent::kMaxStopMinutes)) return fail("Stop timer must be a whole number from 1 to 180 minutes");
            media.stop_minutes=static_cast<uint16_t>(stop->valueint);
        }
        media.app=APP_ID_RADIO;media.source=Source::Radio;
        if (!text_field(args,"city",true,media.name,sizeof(RadioLocation::city),&error) ||
            !text_field(args,"station",false,media.detail,sizeof(media.detail),&error)) return fail(error);
        if (!radio_catalog_known_place(media.name)) return fail("No radio directory for that place; name a Malaysian city, town or state");
    }
    const Result result=execute(request);
    switch(result.error) {
        case Error::Busy:return fail("Device is busy with another request; try again later");
        case Error::Inactive:return fail("Xiaozhi has exited; device control not carried out");
        case Error::Storage:return fail("Could not save the setting; new value not applied. Try again later");
        case Error::Timeout:return fail("Result not confirmed; check device status before deciding to retry");
        case Error::Alarm:return fail("Handle the current reminder before opening another app");
        case Error::UnknownApp:return fail("Cannot open that app");
        case Error::AlreadyOpen:return fail("Xiaozhi is already running; no need to open it again");
        case Error::TimerConflict:return fail("Pomodoro state does not allow this action. Check status first: resume if paused, start if not started or finished. Do not reset an existing timer on your own");
        case Error::TimerInvalid:return fail("Invalid pomodoro action");
        case Error::None:break;
    }
    Json value(cJSON_CreateObject(),cJSON_Delete);
    if (request.action==Action::Pomodoro) {
        cJSON_AddBoolToObject(value.get(),"success",true);
        cJSON_AddStringToObject(value.get(),"phase",timer_phases[result.timer.phase]);
        cJSON_AddStringToObject(value.get(),"status",timer_states[result.timer.status]);
        cJSON_AddNumberToObject(value.get(),"remaining_seconds",result.timer.remaining_seconds);
        cJSON_AddNumberToObject(value.get(),"duration_seconds",pomodoro_duration_us(result.timer.phase)/1000000ULL);
        cJSON_AddNumberToObject(value.get(),"completed_focus_sessions",result.timer.completed);
        cJSON_AddBoolToObject(value.get(),"continues_in_background",true);
        cJSON_AddBoolToObject(value.get(),"persisted",false);
        return reply(value.get(),false);
    }
    cJSON_AddNumberToObject(value.get(),"volume_percent",result.volume);
    cJSON_AddNumberToObject(value.get(),"brightness_percent",result.brightness);
    if (result.applied_brightness<result.brightness) {
        // Saved as asked, shown dimmer until the battery is charged.
        cJSON_AddNumberToObject(value.get(),"applied_brightness_percent",result.applied_brightness);
        cJSON_AddBoolToObject(value.get(),"limited_by_low_battery",true);
    }
    cJSON_AddStringToObject(value.get(),"current_app",app_key(result.app));
    if (request.action==Action::Open || request.action==Action::Handover) {
        const bool handover=request.action==Action::Handover;
        cJSON_AddBoolToObject(value.get(),"accepted",true);
        cJSON_AddStringToObject(value.get(),"target_app",app_key(handover?request.handover.app:request.app));
        cJSON_AddStringToObject(value.get(),"state","switch_after_tool_response");
        if (handover) {
            // Radio resolves the request after the directory lookup; nothing plays yet.
            cJSON_AddBoolToObject(value.get(),"playback_confirmed",false);
            if (request.handover.stop_minutes) cJSON_AddNumberToObject(value.get(),"stop_after_minutes",request.handover.stop_minutes);
        }
    } else if (request.action!=Action::Status) {
        cJSON_AddBoolToObject(value.get(),"saved",true);
        if (request.action==Action::Volume) xiaozhi::ui_set_volume(result.volume);
    }
    return reply(value.get(),false);
}
}
