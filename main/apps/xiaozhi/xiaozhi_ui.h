#pragma once

#include "apps/xiaozhi/xiaozhi_protocol.h"
#include <string>

namespace xiaozhi {

// The UI layer may only be called from the "control task"; every entry point re-validates widget pointers [inside] the LVGL lock.
//
// The old code did `if (!s_status_label || !lock())` before locking: ui_hide() deletes widgets and nulls the pointers while
// holding the lock, and the network task could be preempted after the check passed but before locking - then call
// lv_label_set_text() on a freed lv_obj. That was one source of the "occasional crash mid-conversation"
// bug. Now the check and the use happen under the same lock, so that window no longer exists.

void ui_init();
void ui_show();
void ui_hide();

void ui_set_state(State state);                       // Top status pill
void ui_set_emotion(const char *emotion);              // Server llm.emotion → face
void ui_set_status_text(const std::string &text);      // Custom status text (activation code/error)
void ui_set_user_text(const std::string &text);        // What the user said (replace)
void ui_set_ai_text(const std::string &text);          // Xiaozhi reply (replace)
void ui_append_ai_text(const std::string &text);       // Xiaozhi reply (append)
void ui_set_hint(const std::string &text);             // Bottom button hint
void ui_clear_text();
void ui_set_volume(unsigned percent);

} // namespace xiaozhi
