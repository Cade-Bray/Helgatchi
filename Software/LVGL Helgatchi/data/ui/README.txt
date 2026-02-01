Place LVGL XML assets here for runtime loading (LittleFS path S:/ui).

Expected layout for the LVGL Editor:
- project.xml
- globals.xml
- components/
- screens/
- widgets/

Runtime loader notes:
- globals.xml is loaded from the root.
- components/, widgets/, and screens/ are loaded recursively.

After adding files, upload the filesystem image:
- PlatformIO: "Upload Filesystem Image" task or 'pio run --target uploadfs'.
