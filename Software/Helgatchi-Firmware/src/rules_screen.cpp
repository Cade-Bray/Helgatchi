#include "rules_screen.h"
#include "rules_service.h"
#include "UI/screens.h"
#include "UI/styles.h"
#include <lvgl.h>
#include <Arduino.h>

RulesScreen g_rules_screen;

static EventBus* _bus = nullptr;
static lv_obj_t* _scroll_container = nullptr;
static bool      _all_rules_expanded = false;
static bool      _populate_pending = false;
static bool      s_focus_all_rules_btn = false;
static char      s_tag_storage[RulesService::MAX_TAGS][Rule::MAX_TAG_LEN];

static void _populate_rules_screen();

static void _update_switches_in_place() {
    if (!_scroll_container) return;

    char tags[RulesService::MAX_TAGS][Rule::MAX_TAG_LEN];
    bool tag_enabled[RulesService::MAX_TAGS];
    uint16_t tag_cnt = g_rules.getUniqueTags(tags, tag_enabled, RulesService::MAX_TAGS);

    uint32_t child_cnt = lv_obj_get_child_count(_scroll_container);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(_scroll_container, i);
        if (!row) continue;

        // Rows contain [label, switch]
        if (lv_obj_get_child_count(row) >= 2) {
            lv_obj_t* sw = lv_obj_get_child(row, 1);
            if (!sw || !lv_obj_check_type(sw, &lv_switch_class)) continue;

            const char* user_data = (const char*)lv_obj_get_user_data(sw);
            if (!user_data) continue;

            // Check if user_data matches a tag
            bool is_tag = false;
            for (uint16_t t = 0; t < tag_cnt; t++) {
                if (strcmp(tags[t], user_data) == 0) {
                    if (tag_enabled[t]) lv_obj_add_state(sw, LV_STATE_CHECKED);
                    else                lv_obj_remove_state(sw, LV_STATE_CHECKED);
                    is_tag = true;
                    break;
                }
            }

            // If not a tag, check if it matches a rule name
            if (!is_tag) {
                const uint16_t rule_cnt = g_rules.count();
                for (uint16_t r_idx = 0; r_idx < rule_cnt; r_idx++) {
                    const Rule* r = g_rules.get(r_idx);
                    if (r && strcmp(r->name, user_data) == 0) {
                        if (g_rules.isRuleActive(*r)) lv_obj_add_state(sw, LV_STATE_CHECKED);
                        else                          lv_obj_remove_state(sw, LV_STATE_CHECKED);
                        break;
                    }
                }
            }
        }
    }
}

static void _async_populate_callback(void* /*user_data*/) {
    _populate_pending = false;
    if (lv_screen_active() == objects.screen_template) {
        _populate_rules_screen();
    }
}

static void _request_populate() {
    if (!_populate_pending) {
        _populate_pending = true;
        lv_async_call(_async_populate_callback, nullptr);
    }
}

static void _on_tag_switch_changed(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    const char* tag_name = (const char*)lv_obj_get_user_data(sw);
    if (sw && tag_name) {
        bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
        g_rules.setTagEnabled(tag_name, enabled);
        _update_switches_in_place();
    }
}

static void _on_rule_switch_changed(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    const char* rule_name = (const char*)lv_obj_get_user_data(sw);
    if (sw && rule_name) {
        bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
        g_rules.setEnabled(rule_name, enabled);
        _update_switches_in_place();
    }
}

static void _on_all_rules_button_clicked(lv_event_t* /*e*/) {
    _all_rules_expanded = !_all_rules_expanded;
    s_focus_all_rules_btn = true;
    _request_populate();
}


static void _populate_rules_screen() {
    if (!objects.screen_template || !objects.obj3) return;

    if (objects.obj2__top_bar_center_text) {
        lv_label_set_text(objects.obj2__top_bar_center_text, "Rules");
    }

    // Safely remove objects from navigation group BEFORE deleting container children
    lv_group_remove_all_objs(groups.UINavigation);
    lv_obj_clean(objects.obj3);
    objects.obj38 = nullptr;
    _scroll_container = nullptr;

    _scroll_container = lv_obj_create(objects.obj3);
    lv_obj_set_pos(_scroll_container, 0, 0);
    lv_obj_set_size(_scroll_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_left(_scroll_container, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(_scroll_container, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(_scroll_container, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(_scroll_container, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(_scroll_container, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(_scroll_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(_scroll_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_layout(_scroll_container, LV_LAYOUT_FLEX, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_flex_flow(_scroll_container, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_flex_main_place(_scroll_container, LV_FLEX_ALIGN_START, LV_PART_MAIN | LV_STATE_DEFAULT);

    // --- Section 1: TAG FILTERS ---
    lv_obj_t* tag_hdr = lv_label_create(_scroll_container);
    lv_obj_set_style_text_font(tag_hdr, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tag_hdr, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text_static(tag_hdr, "TAG FILTERS");

    char tags[RulesService::MAX_TAGS][Rule::MAX_TAG_LEN];
    bool tag_enabled[RulesService::MAX_TAGS];
    uint16_t tag_cnt = g_rules.getUniqueTags(tags, tag_enabled, RulesService::MAX_TAGS);

    if (tag_cnt == 0) {
        lv_obj_t* no_tags = lv_label_create(_scroll_container);
        lv_obj_set_style_text_font(no_tags, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(no_tags, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text_static(no_tags, "No rule tags defined");
    } else {
        for (uint16_t i = 0; i < tag_cnt; i++) {
            lv_obj_t* row = lv_obj_create(_scroll_container);
            lv_obj_set_size(row, LV_PCT(100), 44);
            lv_obj_set_style_pad_left(row, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(row, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(row, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_layout(row, LV_LAYOUT_FLEX, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_t* lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(lbl, tags[i]);

            lv_obj_t* sw = lv_switch_create(row);
            add_style_focused___switch(sw);
            if (tag_enabled[i]) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(sw, LV_STATE_CHECKED);
            }

            strncpy(s_tag_storage[i], tags[i], Rule::MAX_TAG_LEN - 1);
            s_tag_storage[i][Rule::MAX_TAG_LEN - 1] = '\0';
            lv_obj_set_user_data(sw, (void*)s_tag_storage[i]);

            lv_obj_add_event_cb(sw, _on_tag_switch_changed, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_group_add_obj(groups.UINavigation, sw);
        }
    }

    // --- Section 2: ALL RULES ---
    lv_obj_t* btn_all_rules = lv_button_create(_scroll_container);
    add_style_focused___button(btn_all_rules);
    lv_obj_set_size(btn_all_rules, LV_PCT(100), 42);
    lv_obj_set_style_margin_top(btn_all_rules, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_all_rules, _on_all_rules_button_clicked, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(groups.UINavigation, btn_all_rules);

    if (s_focus_all_rules_btn) {
        if (groups.UINavigation) {
            lv_group_focus_obj(btn_all_rules);
        }
        lv_obj_scroll_to_view(btn_all_rules, LV_ANIM_OFF);
        s_focus_all_rules_btn = false;
    }

    lv_obj_t* btn_lbl = lv_label_create(btn_all_rules);
    lv_obj_center(btn_lbl);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    char btn_txt[32];
    snprintf(btn_txt, sizeof(btn_txt), "All Rules (%u) %s", g_rules.count(), _all_rules_expanded ? "v" : ">");
    lv_label_set_text(btn_lbl, btn_txt);

    if (_all_rules_expanded) {
        const uint16_t rule_cnt = g_rules.count();
        for (uint16_t i = 0; i < rule_cnt; i++) {
            const Rule* r = g_rules.get(i);
            if (!r) continue;

            lv_obj_t* row = lv_obj_create(_scroll_container);
            lv_obj_set_size(row, LV_PCT(100), 44);
            lv_obj_set_style_pad_left(row, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(row, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(row, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_layout(row, LV_LAYOUT_FLEX, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_t* lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(lbl, r->name);

            lv_obj_t* sw = lv_switch_create(row);
            add_style_focused___switch(sw);
            if (g_rules.isRuleActive(*r)) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(sw, LV_STATE_CHECKED);
            }

            lv_obj_set_user_data(sw, (void*)r->name);
            lv_obj_add_event_cb(sw, _on_rule_switch_changed, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_group_add_obj(groups.UINavigation, sw);
        }
    }
}

static void _on_rules_screen_load(lv_event_t* /*e*/) {
    _populate_rules_screen();
}

void RulesScreen::begin(EventBus& bus) {
    _bus = &bus;
    if (objects.screen_template) {
        lv_obj_add_event_cb(objects.screen_template, _on_rules_screen_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
        lv_obj_add_event_cb(objects.screen_template, [](lv_event_t* /*e*/) {
            _populate_pending = false;
            lv_group_remove_all_objs(groups.UINavigation);
            if (objects.obj3) {
                lv_obj_clean(objects.obj3);
            }
            objects.obj38 = nullptr;
            _scroll_container = nullptr;
        }, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    }
}

void RulesScreen::onEvent(const Event& /*e*/) {
}
