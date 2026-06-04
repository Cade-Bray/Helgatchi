#ifndef EEZ_LVGL_UI_STRUCTS_H
#define EEZ_LVGL_UI_STRUCTS_H

#include "eez-flow.h"

#include <stdint.h>
#include <stdbool.h>

#include "vars.h"

using namespace eez;

enum FlowStructures {
    FLOW_STRUCTURE_SCAN_TYPE_ICON = 16384
};

enum FlowArrayOfStructures {
    FLOW_ARRAY_OF_STRUCTURE_SCAN_TYPE_ICON = 81920
};

enum ScanTypeIconFlowStructureFields {
    FLOW_STRUCTURE_SCAN_TYPE_ICON_NUM_FIELDS
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

#endif /*EEZ_LVGL_UI_STRUCTS_H*/