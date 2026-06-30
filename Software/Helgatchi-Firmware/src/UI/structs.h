#ifndef EEZ_LVGL_UI_STRUCTS_H
#define EEZ_LVGL_UI_STRUCTS_H

#include "eez-flow.h"

#include <stdint.h>
#include <stdbool.h>

#include "vars.h"

using namespace eez;

enum FlowStructures {
    FLOW_STRUCTURE_SCAN_TYPE_ICON = 16384,
    FLOW_STRUCTURE_ALERT = 16385
};

enum FlowArrayOfStructures {
    FLOW_ARRAY_OF_STRUCTURE_SCAN_TYPE_ICON = 81920,
    FLOW_ARRAY_OF_STRUCTURE_ALERT = 81921
};

enum ScanTypeIconFlowStructureFields {
    FLOW_STRUCTURE_SCAN_TYPE_ICON_NUM_FIELDS
};

enum AlertFlowStructureFields {
    FLOW_STRUCTURE_ALERT_FIELD_ID = 0,
    FLOW_STRUCTURE_ALERT_FIELD_TITLE = 1,
    FLOW_STRUCTURE_ALERT_FIELD_TIME_AGO = 2,
    FLOW_STRUCTURE_ALERT_FIELD_TYPE = 3,
    FLOW_STRUCTURE_ALERT_NUM_FIELDS
};

struct ScanTypeIconValue {
    Value value;
    
    ScanTypeIconValue() {
        value = Value::makeArrayRef(FLOW_STRUCTURE_SCAN_TYPE_ICON_NUM_FIELDS, FLOW_STRUCTURE_SCAN_TYPE_ICON, 0);
    }
    
    ScanTypeIconValue(Value value) : value(value) {}
    
    operator Value() const { return value; }
    
    operator bool() const { return value.isArray(); }
};

typedef ArrayOf<ScanTypeIconValue, FLOW_ARRAY_OF_STRUCTURE_SCAN_TYPE_ICON> ArrayOfScanTypeIconValue;
struct AlertValue {
    Value value;
    
    AlertValue() {
        value = Value::makeArrayRef(FLOW_STRUCTURE_ALERT_NUM_FIELDS, FLOW_STRUCTURE_ALERT, 0);
    }
    
    AlertValue(Value value) : value(value) {}
    
    operator Value() const { return value; }
    
    operator bool() const { return value.isArray(); }
    
    int id() {
        return value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_ID].getInt();
    }
    void id(int id) {
        value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_ID] = IntegerValue(id);
    }
    
    const char *title() {
        return value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TITLE].getString();
    }
    void title(const char *title) {
        value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TITLE] = StringValue(title);
    }
    
    const char *time_ago() {
        return value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TIME_AGO].getString();
    }
    void time_ago(const char *time_ago) {
        value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TIME_AGO] = StringValue(time_ago);
    }
    
    const char *type() {
        return value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TYPE].getString();
    }
    void type(const char *type) {
        value.getArray()->values[FLOW_STRUCTURE_ALERT_FIELD_TYPE] = StringValue(type);
    }
};

typedef ArrayOf<AlertValue, FLOW_ARRAY_OF_STRUCTURE_ALERT> ArrayOfAlertValue;

#endif /*EEZ_LVGL_UI_STRUCTS_H*/