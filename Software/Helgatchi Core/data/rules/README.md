# Rule packs

The firmware loads JSON files from `/rules/*.json` on the device's LittleFS filesystem at boot.

- Files in this folder are bundled into the LittleFS image by PlatformIO.
- Update these files and run `pio run -t uploadfs -e seeed_xiao_esp32s3` to update rules without reflashing firmware.

## JSON format

Each file should look like:

```json
{
  "pack": "AXON",
  "rules": [
    { "oui": "D8A01D", "label": "AXON" },
    { "mac": "D8:A0:1D:12:34:56", "label": "AXON 123456" },
    { "oui": "A1B2C3", "label": "SomeLabel" },
    "001122"
  ]
}
```

Notes:
- `oui` is the 24-bit OUI (first 3 bytes) in hex (`"AABBCC"` or `"0xAABBCC"`).
- `mac` is an exact 48-bit address match (`"AA:BB:CC:DD:EE:FF"` or `"AABBCCDDEEFF"`).
- Rules in packs are always loaded as `enabled: true`.
- Pack rules are *not* saved back to NVS.
- If an OUI exists in both a user rule and a pack, the user rule wins.
