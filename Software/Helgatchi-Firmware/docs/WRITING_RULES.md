# Writing rules

The authoritative guide to Helgatchi's rules engine: what a rule is, how
matching works, every field, the pattern/regex language, worked examples, how to
get a rule onto the device, and how to test it. If you're creating or editing a
rule, this is the page to read.

## Contents

1. [Quick reference](#1-quick-reference)
2. [How rules work](#2-how-rules-work)
3. [Rule anatomy — the top-level fields](#3-rule-anatomy--the-top-level-fields)
4. [Criteria — the match fields](#4-criteria--the-match-fields)
5. [Patterns & regex](#5-patterns--regex)
6. [Vendor-name matching](#6-vendor-name-matching)
7. [Worked examples](#7-worked-examples)
8. [A full annotated ruleset](#8-a-full-annotated-ruleset)
9. [Deploying rules](#9-deploying-rules)
10. [Console command reference](#10-console-command-reference)
11. [Testing & validating your rule](#11-testing--validating-your-rule)
12. [Migration note](#12-migration-note)
13. [Gotchas checklist](#13-gotchas-checklist)
14. [Internals (for engine contributors)](#14-internals-for-engine-contributors)

---

## 1. Quick reference

A rule is one JSON file. Minimal shape:

```json
{
  "name": "flock_safety",
  "title": "Flock nearby",
  "type": "ble",
  "criteria": [
    { "oui": ["B4:1E:52"] },
    { "name": [".*flock.*", ".*alpr.*"] }
  ]
}
```

**Top-level fields**

| Field      | Required | Value |
|------------|----------|-------|
| `name`     | yes | unique id, `[a-z0-9_]`, ≤ 55 chars (also the filename) |
| `title`    | no  | text on the alert card (defaults to `name`) |
| `type`     | no  | `ble` \| `wifi` \| `sys` \| `batt` \| `auto` (default: infer) |
| `action`   | no  | `alert` (default) \| `party` |
| `vibe`     | no  | haptic pattern name (`vibe list` on the console) |
| `led`      | no  | LED pattern name (`led list` on the console) |
| `criteria` | yes | array of match clauses (below) |

**Criterion fields**

| Field     | Matches                          | Value |
|-----------|----------------------------------|-------|
| `oui`     | first 3 MAC bytes                | `"B4:1E:52"` |
| `mac`     | full 6-byte MAC                  | `"12:34:56:78:9A:BC"` |
| `mfg`     | BT SIG company id                | `"0x05D2"` |
| `service` | advertised BLE service UUID      | `"180F"`, `"0x180F"`, or full 128-bit |
| `name`    | BLE name / Wi-Fi SSID            | **pattern** |
| `ssid`    | name, Wi-Fi only                 | **pattern** |
| `oui_org` | IEEE vendor name for the OUI     | **pattern** (expanded at load) |
| `mfg_org` | BT SIG company name              | **pattern** (expanded at load) |

**Pattern cheat sheet** (case-insensitive, matches the *whole* value)

| Intent      | Pattern     |
|-------------|-------------|
| equals      | `flock`     |
| contains    | `.*flock.*` |
| starts with | `flock.*`   |
| ends with   | `.*cam`     |
| regex       | `BWL\d-\d*` (in JSON: `"BWL\\d-\\d*"`) |

---

## 2. How rules work

The scan engine publishes every BLE/Wi-Fi sighting into a ring buffer. The rules
service drains that ring and tests each sighting against every **enabled** rule.
When a rule matches, it fires its action (raise an alert, or start party mode).

- **Within a rule, everything is OR.** Any one criterion hitting fires the rule,
  and any one value inside a criterion counts as a hit. There is **no AND**
  inside a rule today — to require two conditions, write two rules.
- **One fire per (rule, sighting).** Matching stops at the first hit per rule.
- **Dedup is per (rule, MAC).** A device that keeps advertising refreshes the
  existing alert's "last seen" instead of stacking duplicates.
- **`type` categorises the alert; it does not gate matching.** A `name` pattern
  is tested against both BLE names and Wi-Fi SSIDs. Only `ssid` is Wi-Fi-gated.
- **Factory beats user.** On a name collision the read-only factory rule wins.
- **Enable/disable is sticky.** Disabling a rule is stored in NVS and survives a
  reboot and even a filesystem reflash.

---

## 3. Rule anatomy — the top-level fields

### `name` (required)

The unique id and the filename (`<name>.json`). Lowercase letters, digits and
underscores only; ≤ 55 characters. Cannot be changed after creation via the
editor (it's the identity).

### `title`

The human string shown on the alert card. Defaults to `name`. May contain
spaces and punctuation. (Over the serial console an underscore becomes a
space — see §10.)

### `type`

The alert category, one of:

| Value | Meaning |
|-------|---------|
| `ble`  | Bluetooth device |
| `wifi` | Wi-Fi device |
| `sys`  | system alert |
| `batt` | battery alert |
| `auto` | infer from the sighting's radio (default if omitted) |

This only labels the resulting alert; it does **not** restrict which sightings a
rule can match.

### `action`

- `alert` (default) — raise/refresh an alert card with the rule's `title`,
  `vibe` and `led`.
- `party` — trigger party mode (rainbow LEDs, haptics, animation); no alert
  card. Used by `party.json`.

### `vibe` / `led`

Names of registered haptic / LED patterns. Omit for the service default. List
the available names on the console with `vibe list` and `led list`.

---

## 4. Criteria — the match fields

`criteria` is an array of single-key objects. Each object names one field and
gives an array of values. There are two kinds of field.

### Literal fields

| Field     | Format | Notes |
|-----------|--------|-------|
| `oui`     | `"B4:1E:52"` | first three MAC octets (24-bit prefix) |
| `mac`     | `"12:34:56:78:9A:BC"` | a single exact device |
| `mfg`     | `"0x05D2"` | 16-bit BT SIG company id from the advert |
| `service` | `"180F"` / `"0x180F"` / `"0000180f-0000-1000-8000-00805f9b34fb"` | a service UUID advertised by the device; short forms are promoted onto the BLE base UUID |

### Pattern fields

| Field     | Matched against | When |
|-----------|-----------------|------|
| `name`    | BLE adv name or Wi-Fi SSID | runtime, per sighting |
| `ssid`    | the name, **only for Wi-Fi** sightings | runtime |
| `oui_org` | the IEEE vendor name of the sighting's OUI | resolved at load |
| `mfg_org` | the BT SIG company name of the mfg id | resolved at load |

`oui_org` / `mfg_org` are matched against the on-device vendor tables once when
the rule loads, and replaced by the concrete OUIs / company ids they resolve to.
They cost nothing on the scan hot path but can expand to many entries (§6).

---

## 5. Patterns & regex

### The one rule

> A pattern is **case-insensitive** and must match the **whole** value.

Full-match (anchored) is the thing people trip on: `flock` means the value *is
exactly* `flock`, not "contains flock". To match part of a value, add `.*`
wildcards yourself.

### The four everyday shapes

| Intent      | Pattern     | Matches                     | Does **not** match |
|-------------|-------------|-----------------------------|--------------------|
| equals      | `flock`     | `flock`, `FLOCK`            | `flock-cam`, `myflock` |
| contains    | `.*flock.*` | `Flock-Cam`, `myFLOCKing`   | `flo`, `lock` |
| starts with | `flock.*`   | `flock`, `flock-cam`        | `myflock` |
| ends with   | `.*cam`     | `body-cam`, `CAM`           | `camera` |

These cost nothing at runtime (they run as plain string compares — §13). Reach
for real regex only when a value has *structure* (a serial number, a suffix).

### Regex tokens

Patterns below are shown **as the matcher sees them**. In a JSON file every
backslash is doubled — `\d` is written `"\\d"` (see [§5 JSON escaping](#writing-patterns-in-json)).

**`.` — any single character**

| Pattern | Matches            | Not |
|---------|--------------------|-----|
| `a.c`   | `abc`, `a5c`, `a-c` | `ac`, `abbc` |

**`* + ?` — repeat the preceding element** (zero-or-more, one-or-more, zero-or-one)

| Pattern   | Matches              | Not |
|-----------|----------------------|-----|
| `ab*c`    | `ac`, `abc`, `abbbc` | `abx` |
| `ab+c`    | `abc`, `abbc`        | `ac` |
| `colou?r` | `color`, `colour`    | `colouur` |

**`\d \D` — digit / non-digit**

| Pattern  | Matches         | Not |
|----------|-----------------|-----|
| `\d\d\d` | `192`, `007`    | `19`, `1a2` |
| `cam\d+` | `cam1`, `cam42` | `cam`, `camx` |
| `\D`     | `x`, `-`        | `5` |

**`\w \W` — word `[A-Za-z0-9_]` / non-word**

| Pattern | Matches          | Not |
|---------|------------------|-----|
| `\w+`   | `node_7`, `AB12` | `a-b`, `a b` |

**`\s \S` — whitespace / non-whitespace**

| Pattern    | Matches   | Not |
|------------|-----------|-----|
| `ap\sname` | `ap name` | `apname` |

**`[...]` `[^...]` — character sets & ranges** (ranges fold case: `[a-z]` also matches `B`)

| Pattern     | Matches      | Not |
|-------------|--------------|-----|
| `[bc]at`    | `bat`, `cat` | `hat` |
| `[0-9a-f]+` | `3f`, `a0b1` | `3g`, `xyz` |
| `[^_]+`     | `abc`        | `a_b` |

**`\` — escape a metacharacter to a literal** (`. * + ? [ ] ( ) { } | ^ $ \`)

| Pattern         | Matches    | Not |
|-----------------|------------|-----|
| `v\d\.\d`       | `v3.1`     | `v3x1`, `v31` |
| `10\.0\.0\.\d+` | `10.0.0.5` | `10x0x0x5` |
| `\*`            | `*`        | `x` |

**`^` `$` — anchors** — redundant (patterns are already fully anchored) but
tolerated. `^flock$` behaves exactly like `flock`.

### What's not supported (and what to do instead)

| Missing            | Instead |
|--------------------|---------|
| alternation `a\|b` | use separate array values: `["a", "b"]` |
| bounded repeat `\d{2,4}` | spell it: `\d\d`, `\d\d\d?\d?`, or loosen to `\d+` |
| capture groups `(ab)+` | flatten the pattern |
| backreferences     | not available |

Rules lean on **arrays** rather than clever single patterns:

```json
{ "name": [".*flock.*", ".*alpr.*", "cam\\d+"] }
```

matches a name containing "flock", **or** "alpr", **or** the regex `cam\d+`.

### Writing patterns in JSON

JSON uses `\` for its own escapes, so a regex backslash must be **doubled**:

| Matcher sees | JSON file |
|--------------|-----------|
| `\d`         | `"\\d"` |
| `\w+`        | `"\\w+"` |
| `BWL\d-\d*`  | `"BWL\\d-\\d*"` |
| `v\.\d`      | `"v\\.\\d"` |

A single `\d` in a file is invalid JSON — the whole file is rejected at load
with a parse error on the console. (The console `rule add` command is *not* JSON;
there you type a single backslash.)

### Limits

Patterns are capped at **64 characters**.

---

## 6. Vendor-name matching

`oui_org` and `mfg_org` match a **pattern against the vendor/company name**, then
expand to every OUI / company id whose name matched — so you can flag a
manufacturer without hand-listing their prefixes.

```json
{ "oui_org": [".*flock.*"] }       // every OUI whose IEEE vendor name contains "flock"
{ "mfg_org": [".*motorola.*"] }    // every BT SIG company containing "motorola"
{ "oui_org": ["sierra wireless"] } // exact vendor-name match (names are suffix-stripped)
```

Because they resolve at load, an overly broad pattern (`.*a.*`) can expand to
hundreds of entries and is capped per rule. Keep them specific. Vendor names have
corporate suffixes stripped ("Apple, Inc." → "Apple"), so match on the brand.

---

## 7. Worked examples

**Regional serial scheme** (the WatchGuard case) — `BWL9-089912`, `BWL5-012241`:

```json
{ "name": ["BWL\\d-\\d*"] }
```

**Body cam labelled `AX` + exactly six digits** (no `{6}`, so spell it):

```json
{ "name": ["AX\\d\\d\\d\\d\\d\\d"] }
```

**Printer with a random hex suffix**, e.g. `HP-3f9a`:

```json
{ "name": ["hp-[0-9a-f]+"] }
```

**Access point on the 5 GHz band** whose SSID ends `_5G`:

```json
{ "ssid": [".*_5g"] }
```

**A device family by several keywords** (contains-any):

```json
{ "name": [".*dashcam.*", ".*bodycam.*", ".*evidence.*"] }
```

**A whole vendor, by name, without listing OUIs:**

```json
{ "oui_org": [".*axon.*"], "mfg_org": [".*axon.*"] }
```

**One exact device (allowlist/party target):**

```json
{ "mac": ["12:34:56:78:9A:BC"] }
```

---

## 8. A full annotated ruleset

JSON has **no comments** — this block is annotated for learning only. A valid,
copy-pasteable version follows.

```jsonc
{
  "name": "acme_surveillance",   // file id — [a-z0-9_], <= 55 chars, unique
  "title": "ACME gear nearby",   // text shown on the alert card
  "vibe": "double_tap",          // haptic pattern (omit for the default; `vibe list`)
  "led": "red_blue",             // LED pattern (omit for the default; `led list`)
  "type": "ble",                 // alert category: ble | wifi | sys | batt | auto
  "action": "alert",             // alert | party
  "criteria": [                  // ANY criterion hitting fires the rule (all OR'd)

    // --- literal fields ---
    { "oui": ["00:1D:96", "B4:1E:52"] },          // match either 24-bit MAC prefix
    { "mac": ["12:34:56:78:9A:BC"] },             // one exact device
    { "mfg": ["0x05D2", "0x0008"] },              // BT SIG company id(s)
    { "service": ["180F", "0x180A",               // advertised service UUID: short...
                  "0000fe2c-0000-1000-8000-00805f9b34fb"] }, // ...or full 128-bit

    // --- pattern fields (case-insensitive, whole-value) ---
    { "name": ["ACME.*",                          // name/SSID starts with "acme"
               ".*bodycam.*",                     // ...or contains "bodycam"
               "AX\\d\\d\\d\\d\\d\\d"] },          // ...or "AX" + exactly six digits
    { "ssid": [".*_5g", "ACME-[0-9]+"] },         // Wi-Fi ONLY: ends "_5g", or "ACME-"+digits
    { "oui_org": [".*acme.*"] },                  // any OUI whose vendor name contains acme
    { "mfg_org": [".*acme.*"] }                   // any BT SIG company containing acme
  ]
}
```

The same rule as a valid file (comments removed, backslashes doubled):

```json
{
  "name": "acme_surveillance",
  "title": "ACME gear nearby",
  "vibe": "double_tap",
  "led": "red_blue",
  "type": "ble",
  "action": "alert",
  "criteria": [
    { "oui": ["00:1D:96", "B4:1E:52"] },
    { "mac": ["12:34:56:78:9A:BC"] },
    { "mfg": ["0x05D2", "0x0008"] },
    { "service": ["180F", "0x180A", "0000fe2c-0000-1000-8000-00805f9b34fb"] },
    { "name": ["ACME.*", ".*bodycam.*", "AX\\d\\d\\d\\d\\d\\d"] },
    { "ssid": [".*_5g", "ACME-[0-9]+"] },
    { "oui_org": [".*acme.*"] },
    { "mfg_org": [".*acme.*"] }
  ]
}
```

---

## 9. Deploying rules

There are two homes for rules, with different install paths.

### Factory rules (shipped in the image)

Read-only seeds under `data/rules/factory/`. To add or change one:

1. Create/edit the `.json` file in `data/rules/factory/`.
2. Syntax-check it before flashing: `python -m json.tool data/rules/factory/your_rule.json`
   (prints the file if valid, or points at the first syntax error). Full
   behavioural testing comes after flashing — see §11.
3. Build and flash the filesystem image:
   ```
   pio run -t buildfs      # build the LittleFS image from data/
   pio run -t uploadfs     # flash it
   ```

Factory rules can't be edited on-device; the firmware never rewrites them.

### User rules (created at runtime)

Writable, stored under `/rules/user/`, auto-persisted on every change and kept
across a firmware-only reflash. Create them either way:

- **WebFlasher** — connect, open the rules card, add a rule with the field +
  match-mode dropdowns, Save. It sends `rule save <json>` for you.
- **Serial console** — `rule save <json>` for a whole rule, or build one up with
  `rule create <name>` then `rule add <name> <field>=<values>` (§10).

A user rule with the same `name` as a factory rule is ignored (factory wins).

### Lifecycle

- `rule reload` — wipe in-memory rules and re-read both directories (also zeroes
  match counters).
- `rule enable <name>` / `rule disable <name>` — sticky, stored in NVS.
- `rule delete <name>` — removes a **user** rule and its file.
- `pio run -t erase` — nuke everything (NVS + filesystem). Only needed to escape
  stale state; not part of normal rule work.

---

## 10. Console command reference

```
rule                          show this help
rule list                     dump every rule with its criteria
rule show <name>              details + live match count for one rule
rule create <name> [k=v ...]  new user rule (k=v: title= vibe= led= type= action=)
rule add <name> <f>=<v>[,<v>] add one or more criteria (space-separated clauses)
rule rm <name> <idx>          remove the Nth criterion
rule delete <name>            delete a user rule
rule enable  <name>           enable  (NVS overlay)
rule disable <name>           disable (NVS overlay)
rule reload                   re-read /rules/{factory,user}; zero counters
rule stats                    match / ring-drain counters
rule dump                     all rules as one JSON line (used by the WebFlasher)
rule save <json>              create/replace a user rule from a JSON object
```

Support commands:

```
led list                      list LED pattern names for `led`
vibe list                     list haptic pattern names for `vibe`
scan inject <k=v>             push a synthetic sighting (test the match path)
scan list                     show the seen-devices map
scan clear                    wipe the seen map
```

**Console input rules** (they differ from JSON):

- `_` becomes a space in string values, so `rule add r oui_org=sierra_wireless`
  matches "Sierra Wireless". A literal underscore can't be typed — use `\w` or
  author via JSON.
- No spaces or commas *inside* a value (they separate tokens / values).
- Backslashes are single here (`name=BWL\d-\d*`), doubled in JSON files.

Example, building a rule by hand:

```
rule create bwl_cams type=ble title=BWL_camera
rule add   bwl_cams name=BWL\d-\d*
rule add   bwl_cams oui_org=watchguard
rule show  bwl_cams
```

---

## 11. Testing & validating your rule

You don't need the real target device present to test a rule. The console can
inject synthetic sightings and report whether your rule fired. Run these in the
serial console or the WebFlasher's console view.

The goal is two checks: your rule **loads clean**, and it **fires on what you
intend — and nothing else**.

### Step 1 — Confirm it loaded

- **Saved via WebFlasher / `rule save`:** a bad rule is rejected on the spot
  (the editor shows an error; `rule save` prints `{"ok":false}`). Fix and resave.
- **Flashed as a factory file:** watch the boot log. You want
  `[rules] loaded N rules`. A malformed file logs a parse error naming the file;
  an unknown/removed field logs `bad criterion field`. Fix and reflash.
- Either way, run `rule show <name>` and read the criteria back. Patterns print
  as `name ~ "..."`. **If a criterion you wrote is missing, it was rejected** —
  usually an invalid pattern or a typo'd field name.

### Step 2 — Zero the counters

```
rule reload
```

This re-reads both rule directories and resets every match counter to 0, so you
can measure from a clean slate. `rule show <name>` now reports `matches: 0`.

### Step 3 — Inject a device that SHOULD match

`scan inject` pushes a fake sighting through the real engine:

```
scan inject domain=bt mac=B4:1E:52:00:00:01 name=Flock-Cam
rule show flock_safety        # expect: matches: 1
```

Two constraints on injected sightings: the `name=` value **cannot contain
spaces**, and avoid `mac=AA:BB:CC:DD:EE:FF` (it triggers the factory party rule).
Set the fields your criteria key on — `mac`/`oui` via `mac=`, `mfg` via `mfg=`,
`name`/`ssid` via `name=`, and `oui_org`/`mfg_org` by injecting a MAC whose
vendor your pattern matches.

### Step 4 — Inject a device that should NOT match

Just as important: prove the rule isn't too broad.

```
scan inject domain=bt mac=12:34:56:78:9A:B0 name=unrelated
rule show flock_safety        # expect: matches: 1  (unchanged)
```

This is where anchoring bugs show up. If you wrote `flock` (equals) but expected
"contains", a name like `Flock-Cam` won't match; if you wrote `.*a.*` you'll see
it match nearly everything.

### Step 5 — See the alert fire (optional)

`scan inject` a matching device, then check the device's alert screen (or the
`alerts` console command) for a card with your `title`, `vibe` and `led`.

### Step 6 — Clean up

```
rule delete <scratch_rule>    # remove any throwaway rules you created
scan clear                    # wipe the injected sightings
```

### Worked example — validating a regex rule end to end

```
rule reload
rule create wgtest
rule add wgtest name=BWL\d-\d*
rule show wgtest                                   # criterion prints: name ~ "BWL\d-\d*"
scan inject domain=bt mac=12:34:56:78:9A:C0 name=BWL9-089912
rule show wgtest                                   # matches: 1
scan inject domain=bt mac=12:34:56:78:9A:C1 name=BWLx-1
rule show wgtest                                   # matches: 1  (no change — \d needs a digit)
scan inject domain=bt mac=12:34:56:78:9A:C2 name=xBWL9-1
rule show wgtest                                   # matches: 1  (no change — anchored)
rule delete wgtest
scan clear
```

### When it doesn't behave

| Symptom | Likely cause |
|---------|--------------|
| Rule missing from `rule list` | JSON parse error at load (check the boot log), or its `name` collides with a factory rule (factory wins) |
| A criterion missing from `rule show` | invalid pattern or unknown field — it was rejected while the rest loaded |
| Never matches | bare literal used where you meant contains (add `.*…*`); or `ssid` used for a BLE device; or wrong field |
| Matches far too much | pattern too broad (e.g. `.*a.*`), or an `oui_org`/`mfg_org` pattern that expanded to a huge vendor set |
| `rule add … name=…` returns `ERR` | the pattern failed validation (e.g. a leading `*`, an unterminated `[`, a trailing `\`) |

---

## 12. Migration note

The pre-regex criterion fields are **removed**:

`name_equals`, `name_contains`, `ssid_equals`, `ssid_contains`,
`oui_org_equals`, `oui_org_contains`, `mfg_org_equals`, `mfg_org_contains`.

Migrate a `_contains` value to `.*value.*` and an `_equals` value to `value`.
Old field names in a stale user file are rejected at load with a
`[rules] ... bad criterion field` log line; the rest of the rule still loads.
Re-author the file or reflash the filesystem image. All factory files already
use the new fields.

---

## 13. Gotchas checklist

- Bare literal = **equals**, not contains. Add `.*…*` for substring.
- Double backslashes in JSON (`"\\d"`); single on the console (`\d`).
- No `|` / `{m,n}` / groups — use array values or spell repeats out.
- No literal spaces or commas in console values; `_` → space on the console.
- `type` categorises the alert; it doesn't gate matching. `ssid` is Wi-Fi-only.
- Patterns ≤ 64 chars; `oui_org`/`mfg_org` must be specific (they expand at load).
- Factory rules are read-only on-device; edit the file and reflash the FS.

---

## 14. Internals (for engine contributors)

- **Storage.** Rules live in PSRAM (`RulesService`, `src/rules_service.cpp`).
  Caps: `MAX_RULES = 32`, `MAX_CRITERIA = 256` per rule. A `name`/`ssid` pattern
  stores its verbatim string plus a classified shape; `oui_org`/`mfg_org` expand
  into atomic `CRIT_OUI` / `CRIT_MFG` entries at add time.
- **Pattern classification.** Each pattern is classified once (`_classifyPattern`)
  into `PAT_EXACT` / `PAT_CONTAINS` / `PAT_PREFIX` / `PAT_SUFFIX` (plain
  case-insensitive string compares) or `PAT_REGEX`. Every shipped factory rule is
  a fast-path shape, so the regex engine rarely runs and a busy area stays under
  ~1% CPU.
- **Regex engine.** `lib/re_lite/` — a small case-insensitive, full-match matcher
  (`. * + ? \d \w \s [ranges]`, anchors). Purpose-built, not a vendored library.
  Recursion/backtracking is bounded by the 64-char pattern cap.
- **Persistence.** User rules serialize to `/rules/user/<name>.json` on mutation;
  the disabled-state overlay is a single NVS blob that survives an FS reflash.
- **Round-trip.** Pattern strings are stored and re-serialized verbatim, so
  save/dump/reload preserve exactly what was written. (`oui_org`/`mfg_org` are the
  exception: they persist as their expanded `oui`/`mfg` lists — the source
  pattern isn't retained. Human-authored factory files keep the `*_org` form
  because the firmware never rewrites them.)
