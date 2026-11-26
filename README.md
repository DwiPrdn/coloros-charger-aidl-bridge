# ColorOS 17 Charger V12 AIDL Bridge (`vendor.oplus.hardware.charger.ICharger`)

A **100% pure NDK AIDL V12** (`libbinder_ndk.so`, statically linked `libstdc++`, zero `libc++_shared.so`) HAL bridge that brings native **ColorOS 17** charging features to **Xiaomi / MediaTek (`tanzanite`, MT6789 Helio G99-Ultra, `main_chg.ko` + `subpmic_charger.ko` + `sc8541_charger.ko`)** — with **zero smali patches, zero framework modifications, and full SELinux Enforcing compatibility**.

---

## Features

1. **VOOC / SuperVOOC 33W Detection & Lockscreen Animation**
   - Implements all **136 `ICharger` V12 AIDL transactions** (`VERSION = 12`, `HASH = fe84d18cb5595f64620e559af7f86413aa09a095`), `CHECK_FLAG_SUPPORT` (`flag=31` → `"0"`), and the fast `GET_CHARGER_INFO_UPDATE` (`flag=30`) JSON telemetry path used by ColorOS 17 `OplusBatteryService`.
   - Distinguishes real adapter classes from `/sys/class/power_supply/usb/{real_type,quick_charge_type,power_max}`:
     - **PD / PPS / HVDCP / 33W Fast Adapters** (`≥ 15W` or `quick_charge_type ≥ 1`) → **SuperVOOC (`uiIconType = 3`)**, `33W`, `fastCharge = 1`, `isSupportSpeedCharge = 1` (enables the lockscreen **"Hold to speed up"** button).
     - **Standard DCP Wall Chargers** → **VOOC (`uiIconType = 2`)**.
     - **PC USB (`SDP`, 500 mA)** → normal USB charging (`uiIconType = 0`, `fastCharge = 0`).
   - Computes real Battery Health (`SOH`) from `bms/charge_full` against rated design capacity (`5500 mAh`), ignoring bogus low `charge_full_design` gauge values.

2. **True Hardware Bypass Charging (on a non-bypass phone)**
   - Triggered natively from ColorOS **Battery Settings → Bypass Charging** (`SET_CHG_BYPASS`, `flag = 25`, `"1+switch=1"` / `"1+switch=0"`).
   - **Never uses `input_suspend`** (which cuts VBUS Hi-Z and drains the battery). Instead writes **`smart_chg = 65553`** (`(1 << 16) | (1 << 4) | 1`, `ENDURANCE` functype `4`, `target_soc = 1%`) which votes `ENDURANCE_VOTER = 0 mA` on `TOTAL_FCC` → stops the `sc8541` 2:1 charge pump and sets `sc_chg_set_ichg(0)` while keeping the **VBUS → VSYS power path active**.
   - At `SOC ≥ 80%`, also engages `night_charging = 1` to physically open the battery charging FET (`charger_set_chg(0)`).
   - Verified live on hardware: battery current drops from `-1852 mA` to **`-1.4 mA`** within 2 seconds while `AC powered: true` powers the device directly from USB.
   - Turning bypass OFF (`"1+switch=0"`) cleanly restores any active Charging Limit, or writes `smart_chg = 16` (`0x10`) + `night_charging = 0` to resume normal charging immediately.

3. **Charging Limit (80% or Custom %)**
   - Triggered natively from ColorOS **Battery Health → Charging Limit** (`SET_CHG_UP_LIMIT_STATE` `flag = 14` `"2++<en>+<limit>"`, `SET_CHG_UP_LIMIT_VALUE` `flag = 5` `"4++<en>+<limit>+..."`, and legacy `setPsyMmiChgEn`).
   - Writes `smart_chg = (limitPct << 16) | 0x11` (e.g. `5242897` for 80%) using `main_chg.ko`'s built-in 5% recharge hysteresis, plus `night_charging = 1` when configured to 80%.

4. **Functional Lockscreen "Hold to Speed Up" + 42–45°C Thermal Safety Guard**
   - **Normal Mode (default, including when Settings `mode=3` Smart Rapid Charging is enabled)**: caps charging via `constant_charge_current_max = 6` (**3000 mA peak**, keeping charging cool in the 1000–3000 mA range).
   - **Lockscreen "Hold to speed up" (`setCustomSelectChgMode(mode = 4, enable = true)`)**: activates a **10-minute (600 s)** high-speed window, writing level `0` (**6000 mA PD cap → ~5.6 A peak** on the 33W 2:1 charge pump) to both `constant_charge_current_max` and `charge_control_limit` (300 ms supervisor cadence while boosting). Auto-cancels on timer expiry, unplug, stop notification (`mode = 4/0, enable = false`), or enabling Bypass Charging.
   - **Thermal Safety Guard (always active in BOTH Normal and Boost modes, 1.5°C recovery hysteresis)**:

| Battery Temp | Normal Mode Cap | Boost ("Hold to speed up") Cap | Recovery Threshold |
| :--- | :--- | :--- | :--- |
| `< 40.0°C` | **3000 mA** (lvl `6`) | **~5600 mA** (lvl `0`, 6000 mA PD cap) | — |
| `40.0–41.9°C` | **3000 mA** (lvl `6`) | **4500 mA** (lvl `3`, warm taper) | `< 38.5°C` → full |
| `42.0–42.9°C` | **2500 mA** (lvl `8`) | **2500 mA** (lvl `8`) | `< 40.5°C` |
| `43.0–43.9°C` | **1900 mA** (lvl `11`) | **1900 mA** (lvl `11`) | `< 41.5°C` |
| `44.0–44.9°C` | **1500 mA** (lvl `12`) | **1500 mA** (lvl `12`) | `< 42.5°C` |
| `≥ 45.0°C` | **900 mA** (lvl `13`) | **900 mA** (lvl `13`) | `< 43.5°C` |

---

## Repository Layout

```text
├── build.sh                                                   # NDK r27c build script (zero libc++_shared.so)
├── src/
│   ├── charger_bridge.cpp                                     # Pure AIDL V12 ICharger HAL bridge + supervisor
│   └── bridge_log.h                                           # Runtime debug toggle (persist.vendor.cos.bridge.debug)
├── vendor/odm/
│   ├── bin/hw/vendor.oplus.hardware.charger-V6-service        # Prebuilt arm64 V12 binary (named V6 for existing file_contexts)
│   ├── etc/init/vendor.oplus.hardware.charger-V6-service.rc   # Service definition + battery sysfs chown/chmod
│   ├── etc/vintf/manifest/manifest_oplus_charger_aidl.xml     # VINTF AIDL V12 manifest
│   └── etc/permissions/vendor-oplus-hardware-charger.xml      # Maps shared library to V12-java.jar
└── config/
    ├── vendor_fs_config.snippet
    ├── vendor_file_contexts.snippet
    └── selinux_enforcing.cil.snippet
```

> **Why is the binary named `vendor.oplus.hardware.charger-V6-service`?**
> Most ported ColorOS vendor images already label `vendor.oplus.hardware.charger-V6-service` as `u:object_r:hal_charger_oplus_exec:s0` in `vendor_file_contexts`. Keeping that filename while serving full **AIDL V12** (`VERSION = 12`, manifest `<version>12</version>`) lets it drop in with zero `file_contexts` or `precompiled_sepolicy` changes.

---

## Building from Source

```bash
export NDK_DIR=/path/to/android-ndk-r27c
export BINDER_NDK=/path/to/coloros/system/lib64/libbinder_ndk.so
./build.sh
```

Verify the resulting binary only links standard NDK/LL-NDK libraries (`libbinder_ndk.so`, `liblog.so`, `libm.so`, `libdl.so`, `libc.so` — **no `libc++_shared.so`**).

---

## Installation (ROM / `vendor.img`)

1. Copy all 4 files under `vendor/odm/` into your unpacked `vendor/odm/` partition:
   - `vendor/odm/bin/hw/vendor.oplus.hardware.charger-V6-service` (`0 2000 0755`, `u:object_r:hal_charger_oplus_exec:s0`)
   - `vendor/odm/etc/init/vendor.oplus.hardware.charger-V6-service.rc` (`0 0 0644`, `u:object_r:vendor_configs_file:s0`)
   - `vendor/odm/etc/vintf/manifest/manifest_oplus_charger_aidl.xml` (`0 0 0644`, `u:object_r:vendor_configs_file:s0`)
   - `vendor/odm/etc/permissions/vendor-oplus-hardware-charger.xml` (`0 0 0644`, `u:object_r:vendor_configs_file:s0`)
2. Ensure the SELinux rules from `config/selinux_enforcing.cil.snippet` are present in `vendor/etc/selinux/vendor_sepolicy.cil` (and `vendor_service_contexts` maps `vendor.oplus.hardware.charger.ICharger/default` → `u:object_r:hal_charger_service:s0`).
3. Ensure ColorOS Battery UI features are enabled in `/my_product/etc/extension/com.oplus.app-features.xml`:
   ```xml
   <app_feature name="os.charge.settings.smartchargeswitch.open"/>
   <app_feature name="os.charge.settings.batterysettings.batteryhealth"/>
   <app_feature name="com.oplus.plc_charge.support"/>
   <app_feature name="com.oplus.fullscene_plc_charge.support" args="boolean:true"/>
   ```
4. Repack and flash `vendor.img`.

---

## Verification & Self-Test

### 1. Built-in dry-run self-test (28 checks, zero sysfs writes)
```bash
adb push vendor/odm/bin/hw/vendor.oplus.hardware.charger-V6-service /data/local/tmp/charger_test
adb shell "chmod 755 /data/local/tmp/charger_test && /data/local/tmp/charger_test --self-test; rm -f /data/local/tmp/charger_test"
```

### 2. Live service verification
```bash
# Confirm running in SELinux Enforcing
adb shell "getenforce; ps -AZ | grep charger-V6"

# Confirm AIDL interface version == 12 (0x0c)
adb shell "service call vendor.oplus.hardware.charger.ICharger/default 16777215"

# Inspect live ColorOS battery state (with fast charger plugged in)
adb shell "dumpsys battery"
# Expected: ChargerTechnology: 3, ChargeFastCharger: true, UsbHwStatus: 0, BatteryHwStatus: 0

# Inspect live JSON telemetry
adb shell "service call vendor.oplus.hardware.charger.ICharger/default 128 i32 30 s16 Update i32 1"
```

### 3. Live verbose logging (no reboot needed)
By default only key mode transitions (Bypass ON/OFF, Charging Limit ON/OFF, Hold-to-Speed-Up ON/OFF, Thermal Guard tier changes) and errors are logged. Enable full verbose logging live:
```bash
adb shell setprop persist.vendor.cos.bridge.debug 1
adb logcat -s ChargerAidlBridge:V
# Silence verbose logs again:
adb shell setprop persist.vendor.cos.bridge.debug 0
```

---

## Troubleshooting

| Symptom | Cause | Fix |
| :--- | :--- | :--- |
| **No VOOC / SuperVOOC animation**, `dumpsys battery` shows `ChargerTechnology: 0` | `OplusBatteryService` expects `CHECK_FLAG_SUPPORT` (`flag=31`) to return `"0"` (not `"1"`), or VINTF manifest still declares `<version>6</version>`, or charger is slow PC USB (`SDP`). | Confirm `manifest_oplus_charger_aidl.xml` has `<version>12</version>`, check `service call ... 16777215` returns `0x0c`, and plug in a wall PD/QC/DCP adapter (not a PC USB port). |
| **"Hold to speed up" shows on lockscreen, but current stays low** | (1) Battery SOC is near `95–100%` — the kernel's `hq_cp_policy.ko` blocks the 2:1 `sc8541` charge pump when SOC is too high (`soc too high`), falling back to buck-only (~1.85A). (2) Battery temp is `≥ 42°C` so the thermal guard is actively cooling it. (3) Sysfs nodes lack `system:system 0664` ownership. | Test below ~80–85% SOC and `< 40°C`; verify `service call ... 22` returns `4` and `service call ... 18` returns `0` while boosting; check `vendor.oplus.hardware.charger-V6-service.rc` ran `chown system system` on `constant_charge_current_max` and `charge_control_limit`. |
| **Bypass Charging toggle missing in Battery Settings** | Missing `com.oplus.fullscene_plc_charge.support` app feature XML. | Add `<app_feature name="com.oplus.fullscene_plc_charge.support" args="boolean:true"/>` to `/my_product/etc/extension/com.oplus.app-features.xml`. |
| **Bypass Charging toggled ON, but battery still charges** | `/sys/class/power_supply/battery/smart_chg` not writable by `system:system`, or `hal_charger_oplus` lacks `sysfs_battery_supply` write SELinux rule, or current SOC is below your Bypass trigger % (`bypass_charge_value`). | Check `logcat -s ChargerAidlBridge:V` for `Applied smart_chg=0x00010011 (65553)` and check `logcat -d \| grep avc:.*hal_charger`. Ensure current battery % ≥ the Bypass threshold slider in Settings. |
| **Java apps fail loading `vendor.oplus.hardware.charger` library** | Stock `vendor-oplus-hardware-charger.xml` still references non-existent `V6-java.jar`. | Install the included `vendor/odm/etc/permissions/vendor-oplus-hardware-charger.xml` pointing to `vendor.oplus.hardware.charger-V12-java.jar`. |

---

## License

MIT — see [LICENSE](LICENSE).
