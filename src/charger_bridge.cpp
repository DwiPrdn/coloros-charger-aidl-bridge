// ColorOS 17 ICharger V12 AIDL HAL Bridge for Redmi Note 14 4G (tanzanite, MT6789)
// Pure NDK libbinder_ndk implementation — ZERO framework patches, ZERO smali, ZERO libc++_shared.
//
// Features implemented directly against tanzanite's main_chg.ko + subpmic_charger.ko + sc8541_charger.ko:
//   1. VOOC / SuperVOOC detection & accurate wattage/status reporting (33W SuperVOOC / VOOC + lockscreen anim)
//   2. True Hardware Bypass Charging (SET_CHG_BYPASS flag=25):
//        Uses smart_chg ENDURANCE functype=4 (target_soc=1 -> ENDURANCE_VOTER=0 mA on TOTAL_FCC ->
//        sc_chg_set_ichg(0) while keeping VBUS->VSYS power path ON!) + night_charging=1 when SOC>=80%.
//   3. Charging Limit (SET_CHG_UP_LIMIT_STATE flag=14, SET_CHG_UP_LIMIT_VALUE flag=5, setPsyMmiChgEn):
//        Uses smart_chg ENDURANCE functype=4 with target_soc=limitPct (5% recharge hysteresis) +
//        night_charging=1 when limitPct==80.
//   4. Functional Lockscreen "Hold to speed up" + Thermal Safety Guard:
//        - Normal (default): caps charging via constant_charge_current_max=6 (3000 mA peak -> 1000-3000 mA).
//        - Boosted (setCustomSelectChgMode(3, true) from lockscreen hold): boosts up to ~5600 mA (level 0,
//          6000 mA PD table cap / 33W 2:1 charge pump peak) for 10 minutes (600 s), overriding
//          charge_control_limit while temp < 42 C.
//        - Thermal Safety Guard (42-45 C with 1.5 C recovery hysteresis): automatically steps down BOTH
//          constant_charge_current_max and charge_control_limit so the battery always cools safely:
//            < 40.0 C : full target (lvl 0 boosted / lvl 6 normal)
//            40-41.9C : lvl 3 (4500 mA) boosted / lvl 6 (3000 mA) normal
//            42-42.9C : lvl 8 (2500 mA)
//            43-43.9C : lvl 11 (1800 mA)
//            44-44.9C : lvl 12 (1500 mA)
//            >= 45.0C : lvl 13 (900 mA)

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "ChargerAidlBridge"
#include "bridge_log.h"

// Platform LL-NDK libbinder_ndk symbols
extern "C" {
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_startThreadPool(void);
void ABinderProcess_joinThreadPool(void);
void AIBinder_markVintfStability(AIBinder* binder);
}

// ============================================================================
// AIDL V12 vendor.oplus.hardware.charger.ICharger Constants
// Extracted directly from ColorOS 17 vendor.oplus.hardware.charger-V12-java.jar
// ============================================================================

static constexpr const char* kIChargerDescriptor = "vendor.oplus.hardware.charger.ICharger";
static constexpr const char* kIChargerInstance = "vendor.oplus.hardware.charger.ICharger/default";
static constexpr int32_t kIChargerVersion = 12;
static constexpr const char* kIChargerHash = "fe84d18cb5595f64620e559af7f86413aa09a095";

// ChgFuncFlag constants (vendor.oplus.hardware.charger.ChgFuncFlag)
enum ChgFuncFlag : int32_t {
    FLAG_GET_UI_CHG_ICON_TYPE           = 0,
    FLAG_GET_UI_POWER_VALUE             = 1,
    FLAG_GET_DEVICE_POWER_VALUE         = 2,
    FLAG_GET_ADAPTER_POWER_VALUE        = 3,
    FLAG_GET_CPA_POWER_VALUE            = 4,
    FLAG_SET_CHG_UP_LIMIT_VALUE         = 5,
    FLAG_IS_SUPPORT_SINGLE_HYPER_SPEED  = 6,
    FLAG_GET_BATT_EXPANSION_STATE       = 7,
    FLAG_IS_SUPPORT_ECO_DESIGN          = 8,
    FLAG_GET_BATT_MANU_DATE             = 9,
    FLAG_GET_BATT_FIRST_USAGE_DATE      = 10,
    FLAG_SET_BATT_FIRST_USAGE_DATE      = 11,
    FLAG_CLEAR_BATT_ECO_DATA            = 12,
    FLAG_SET_RECHG_SOC_EN               = 13,
    FLAG_SET_CHG_UP_LIMIT_STATE         = 14,
    FLAG_GET_BATTERY_STATUS_PM_VPH      = 15,
    FLAG_GET_BATTERY_STATUS_PM_VBAT     = 16,
    FLAG_GET_DEEP_DISCHG_COUNTS_VALUE   = 17,
    FLAG_GET_ANTI_EXPANSION_INFO        = 18,
    FLAG_SET_ANTI_EXPANSION_INFO        = 19,
    FLAG_GET_BATT_UI_CYCLE_COUNT        = 20,
    FLAG_GET_BATTERY_TYPE               = 21,
    FLAG_WIRELESS_PENCIL_QCALI          = 22,
    FLAG_GET_BATTERY_GAUGE_TYPE         = 23,
    FLAG_GET_CHG_BYPASS_STATUS          = 24,
    FLAG_SET_CHG_BYPASS                 = 25,
    FLAG_GET_RX_DISABLE_STATUS          = 26,
    FLAG_SET_RX_DISABLE                 = 27,
    FLAG_GET_BATTERY_SEC_IC_TEST_STATUS = 28,
    FLAG_GET_CHARGER_INFO               = 29,
    FLAG_GET_CHARGER_INFO_UPDATE        = 30,
    FLAG_CHECK_FLAG_SUPPORT             = 31,
    FLAG_SET_CHG_PATH                   = 32,
    FLAG_GET_BATTERY_GAUGE_CAR_C        = 33,
    FLAG_SET_BT_INFO                    = 34,
    FLAG_GET_BDD_LEAK_CURR              = 35,
    FLAG_SET_REVERSE_CHG_INFO           = 36,
    FLAG_GET_REVERSE_CHG_INFO           = 37,
    FLAG_SET_WLSPEN_INFO                = 38,
    FLAG_SET_WLSPEN_DISCHG_SOC          = 39,
    FLAG_GET_VDM_INFO                   = 40,
    FLAG_SET_PENCIL_TX_CHARGE_DISABLE   = 41,
    FLAG_SET_BS_BLIND_CFG               = 42,
    FLAG_GET_BS_BLIND_CFG               = 43,
    FLAG_GET_POWERBANK_STATUS           = 44,
    FLAG_GET_WLS_POWERBANK_INFO         = 45,
    FLAG_GET_WLS_POWERBANK_STATUS       = 46,
    FLAG_GET_POWERBANK_INFO             = 47,
};

// All 136 ICharger V12 transaction codes
enum IChargerTx : transaction_code_t {
    TX_VolDividerIcWorkModeSet         = 1,
    TX_chgExchangeMesgInit             = 2,
    TX_chgExchangeSohMesgInit          = 3,
    TX_getAcType                       = 4,
    TX_getBattAuthenticate             = 5,
    TX_getBattPPSChgIng                = 6,
    TX_getBattPPSChgPower              = 7,
    TX_getBattParamNoplug              = 8,
    TX_getBattShortIcOtpStatus         = 9,
    TX_getBattSubCurrent               = 10,
    TX_getBattVoocChgIng               = 11,
    TX_getBatteryVoltageNow            = 12,
    TX_getBccCsvData                   = 13,
    TX_getBccExpStatus                 = 14,
    TX_getBmsHeatingRunningStatus      = 15,
    TX_getBmsHeatingStatus             = 16,
    TX_getChargerControl               = 17,
    TX_getChargerCoolDown              = 18,
    TX_getChargerCriticalLog           = 19,
    TX_getChargerIdVolt                = 20,
    TX_getChargerLog                   = 21,
    TX_getCustomSelectChgMode          = 22,
    TX_getDevinfoFastchg               = 23,
    TX_getFastCharge                   = 24,
    TX_getParallelChgMosTestResult     = 25,
    TX_getPsyAcOnline                  = 26,
    TX_getPsyBatteryCC                 = 27,
    TX_getPsyBatteryCurrentNow         = 28,
    TX_getPsyBatteryFcc                = 29,
    TX_getPsyBatteryHmac               = 30,
    TX_getPsyBatteryLevel              = 31,
    TX_getPsyBatteryNotify             = 32,
    TX_getPsyBatteryPchg               = 33,
    TX_getPsyBatteryPchgResetCount     = 34,
    TX_getPsyBatteryRm                 = 35,
    TX_getPsyBatteryShortFeature       = 36,
    TX_getPsyBatteryShortStatus        = 37,
    TX_getPsyBatteryStatus             = 38,
    TX_getPsyBatteryTemp               = 39,
    TX_getPsyChargeTech                = 40,
    TX_getPsyFastChgType               = 41,
    TX_getPsyInputCurrent              = 42,
    TX_getPsyOtgOnline                 = 43,
    TX_getPsyOtgSwitch                 = 44,
    TX_getPsyPcPortOnline              = 45,
    TX_getPsyQGVbatDeviation           = 46,
    TX_getPsyTypeOrientation           = 47,
    TX_getPsyUsbOnline                 = 48,
    TX_getPsyUsbStatus                 = 49,
    TX_getPsyWirelessRX                = 50,
    TX_getPsyWirelessRxVersion         = 51,
    TX_getPsyWirelessTX                = 52,
    TX_getPsyWirelessTxVersion         = 53,
    TX_getQgVbatDeviation              = 54,
    TX_getQuickModeGain                = 55,
    TX_getReserveSocDebug              = 56,
    TX_getSmartChgMode                 = 57,
    TX_getUIsohValue                   = 58,
    TX_getUisohDebugParameterInfo      = 59,
    TX_getUsbInputCurrentNow           = 60,
    TX_getUsbPrimalType                = 61,
    TX_getWiredOtgOnline               = 62,
    TX_getWirelessAdapterPower         = 63,
    TX_getWirelessCapacity             = 64,
    TX_getWirelessChargePumpEn         = 65,
    TX_getWirelessCurrentNow           = 66,
    TX_getWirelessDeviated             = 67,
    TX_getWirelessOnline               = 68,
    TX_getWirelessPenPresent           = 69,
    TX_getWirelessPtmcId               = 70,
    TX_getWirelessRXEnable             = 71,
    TX_getWirelessRealType             = 72,
    TX_getWirelessTXEnable             = 73,
    TX_getWirelessUserSleepMode        = 74,
    TX_getWirelessVoltageNow           = 75,
    TX_healthd_update_ui_soc_decimal   = 76,
    TX_nightstandby                    = 77,
    TX_notifyScreenStatus              = 78,
    TX_queryChargeInfo                 = 79,
    TX_setChargeEMMode                 = 80,
    TX_setChargerControl               = 81,
    TX_setChargerCoolDown              = 82,
    TX_setChargerCriticalLog           = 83,
    TX_setChargerCycle                 = 84,
    TX_setChargerFactoryModeTest       = 85,
    TX_setChargerLog                   = 86,
    TX_setChgStatusToBcc               = 87,
    TX_setCustomSelectChgMode          = 88,
    TX_setFastchgFwUpdate              = 89,
    TX_setPsyMmiChgEn                  = 90,
    TX_setPsyOtgSwitch                 = 91,
    TX_setReserveSocDebug              = 92,
    TX_setShipMode                     = 93,
    TX_setSmartChgMode                 = 94,
    TX_setSmartCoolDown                = 95,
    TX_setTbattPwrOff                  = 96,
    TX_setUisohDebugInfo               = 97,
    TX_setUsbPrimalType                = 98,
    TX_setWirelessChargePumpEn         = 99,
    TX_setWirelessFtmMode              = 100,
    TX_setWirelessIconDelay            = 101,
    TX_setWirelessIdtAdcTest           = 102,
    TX_setWirelessPenSoc               = 103,
    TX_setWirelessRXEnable             = 104,
    TX_setWirelessTXEnable             = 105,
    TX_setWirelessUserSleepMode        = 106,
    TX_setWlsThirdPartitionInfo        = 107,
    TX_testKitFeatureTest              = 108,
    TX_testKitGetFeatureList           = 109,
    TX_testKitGetFeatureName           = 110,
    TX_testKitGetFeatureNum            = 111,
    TX_updateUiSohToPartion            = 112,
    TX_queryWlsPencilInfo              = 113,
    TX_getChgOlcConfig                 = 114,
    TX_setChgOlcConfig                 = 115,
    TX_setSuperEnduranceStatus         = 116,
    TX_setSuperEnduranceCount          = 117,
    TX_setBobStatus                    = 118,
    TX_setPsySlowChgEn                 = 119,
    TX_getCpVbatDeviation              = 120,
    TX_setBatteryLogPush               = 121,
    TX_getChargingModeInGsmCall        = 122,
    TX_setChargingModeInGsmCall        = 123,
    TX_setChgRusConfig                 = 124,
    TX_getPsyBatterySN                 = 125,
    TX_getBattGaugeInfo                = 126,
    TX_setChgConfig                    = 127,
    TX_getChgConfig                    = 128,
    TX_setUsbEyeDiagram                = 129,
    TX_getUsbCurrentEyeDiagram         = 130,
    TX_queryRgcpHboostInfo             = 131,
    TX_setCV                           = 132,
    TX_getDeepSleepClustersSleepWake   = 133,
    TX_getPowerbankInfo                = 134,
    TX_getInterfaceHash                = 16777214,
    TX_getInterfaceVersion             = 16777215,
};

// ============================================================================
// tanzanite Kernel Sysfs Paths & Constants
// Verified from main_chg.ko + dtbo.img (tanzanite OS2.0.214.0.VOGEUXM)
// ============================================================================

static constexpr const char* kSysBattCapacity    = "/sys/class/power_supply/battery/capacity";
static constexpr const char* kSysBattStatus      = "/sys/class/power_supply/battery/status";
static constexpr const char* kSysBattTemp        = "/sys/class/power_supply/battery/temp";
static constexpr const char* kSysBattVolt        = "/sys/class/power_supply/battery/voltage_now";
static constexpr const char* kSysBattCurr        = "/sys/class/power_supply/battery/current_now";
static constexpr const char* kSysBattChargeFull  = "/sys/class/power_supply/battery/charge_full";
static constexpr const char* kSysBattDesignFull  = "/sys/class/power_supply/battery/charge_full_design";
static constexpr const char* kSysBattChargeCnt   = "/sys/class/power_supply/battery/charge_counter";
static constexpr const char* kSysBattCycleCount  = "/sys/class/power_supply/battery/cycle_count";
static constexpr const char* kSysBattSmartChg    = "/sys/class/power_supply/battery/smart_chg";
static constexpr const char* kSysBattNightChg    = "/sys/class/power_supply/battery/night_charging";
static constexpr const char* kSysBattConstCurr   = "/sys/class/power_supply/battery/constant_charge_current_max";
static constexpr const char* kSysBattThermalLim  = "/sys/class/power_supply/battery/charge_control_limit";

static constexpr const char* kSysUsbOnline       = "/sys/class/power_supply/usb/online";
static constexpr const char* kSysUsbType         = "/sys/class/power_supply/usb/type";
static constexpr const char* kSysUsbRealType     = "/sys/class/power_supply/usb/real_type";
static constexpr const char* kSysUsbQuickType    = "/sys/class/power_supply/usb/quick_charge_type";
static constexpr const char* kSysUsbPowerMax     = "/sys/class/power_supply/usb/power_max";
static constexpr const char* kSysUsbVolt         = "/sys/class/power_supply/usb/voltage_now";
static constexpr const char* kSysUsbCurr         = "/sys/class/power_supply/usb/current_now";
static constexpr const char* kSysUsbInputCurr    = "/sys/class/power_supply/usb/input_current_now";
static constexpr const char* kSysUsbCcOrient     = "/sys/class/power_supply/usb/typec_cc_orientation";
static constexpr const char* kSysUsbOtg          = "/sys/class/power_supply/usb/usb_otg";

static constexpr const char* kSysBmsCapacity     = "/sys/class/power_supply/bms/capacity";
static constexpr const char* kSysBmsTemp         = "/sys/class/power_supply/bms/temp";
static constexpr const char* kSysBmsVolt         = "/sys/class/power_supply/bms/voltage_now";
static constexpr const char* kSysBmsCurr         = "/sys/class/power_supply/bms/current_now";
static constexpr const char* kSysBmsChargeFull   = "/sys/class/power_supply/bms/charge_full";
static constexpr const char* kSysBmsDesignFull   = "/sys/class/power_supply/bms/charge_full_design";
static constexpr const char* kSysBmsChargeCnt    = "/sys/class/power_supply/bms/charge_counter";
static constexpr const char* kSysBmsCycleCount   = "/sys/class/power_supply/bms/cycle_count";
static constexpr const char* kSysBmsSoh          = "/sys/class/power_supply/bms/soh";
static constexpr const char* kSysBmsTimeToFull   = "/sys/class/power_supply/bms/time_to_full_now";
static constexpr const char* kSysBmsAuthentic    = "/sys/class/power_supply/bms/authentic";

// smart_chg bit packing (main_chg.ko smart_chg_store):
//   val = (target_soc << 16) | (1 << 4) | (enable ? 1 : 0)
//   bit 4 (0x10) -> ubfx(1,15)=0x8 -> find_next_bit=3 -> functype=4 (ENDURANCE at offset 0x444)
static constexpr uint32_t packSmartChgEndurance(bool enable, int targetSoc) {
    uint32_t soc = static_cast<uint32_t>(std::max(0, std::min(100, targetSoc)));
    return (soc << 16) | (1u << 4) | (enable ? 1u : 0u);
}

static constexpr uint32_t kSmartChgBypassOn  = packSmartChgEndurance(true, 1);   // 0x00010011 = 65553
static constexpr uint32_t kSmartChgDisable   = packSmartChgEndurance(false, 0);  // 0x00000010 = 16

// dtbo.img hq,pd-thermal-mitigation (mA):
//   [0]=6000, [1]=5400, [2]=5000, [3]=4500, [4]=4000, [5]=3500, [6]=3000, [7]=2700,
//   [8]=2500, [9]=2300, [10]=2100, [11]=1800, [12]=1500, [13]=900, [14]=500, [15]=300
static constexpr int kLevelBoostMax       = 0;   // 6000 mA cap -> ~5600 mA 33W HW peak
static constexpr int kLevelBoostWarm40    = 3;   // 4500 mA (40.0-41.9 C pre-warm taper)
static constexpr int kLevelNormalPeak     = 6;   // 3000 mA peak (1000-3000 mA normal range)
static constexpr int kLevelThermal42      = 8;   // 2500 mA (42.0-42.9 C)
static constexpr int kLevelThermal43      = 11;  // 1800 mA (43.0-43.9 C)
static constexpr int kLevelThermal44      = 12;  // 1500 mA (44.0-44.9 C)
static constexpr int kLevelThermal45      = 13;  //  900 mA (>= 45.0 C cool-down protection)

static constexpr int64_t kBoostDurationSec = 600; // 10 minutes
static constexpr int kDeviceMaxWattage     = 33;  // tanzanite 33W Turbo/SuperVOOC

namespace {

// ============================================================================
// Parcel Helpers
// ============================================================================

struct ParcelableWriter {
    AParcel* p;
    int32_t startPos;
    explicit ParcelableWriter(AParcel* parcel, bool writeNonNull = true) : p(parcel) {
        if (writeNonNull) AParcel_writeInt32(p, 1);
        startPos = AParcel_getDataPosition(p);
        AParcel_writeInt32(p, 0);
    }
    void finish() {
        int32_t endPos = AParcel_getDataPosition(p);
        AParcel_setDataPosition(p, startPos);
        AParcel_writeInt32(p, endPos - startPos);
        AParcel_setDataPosition(p, endPos);
    }
};

static void writeOkStatus(AParcel* out) {
    AStatus* st = AStatus_newOk();
    AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
}

static void writeOkInt(AParcel* out, int32_t v) {
    writeOkStatus(out);
    AParcel_writeInt32(out, v);
}

static void writeOkStr(AParcel* out, const std::string& s) {
    writeOkStatus(out);
    AParcel_writeString(out, s.c_str(), static_cast<int32_t>(s.size()));
}

static std::string readStr(const AParcel* in) {
    std::string result;
    auto alloc = [](void* data, int32_t len, char** buf) -> bool {
        auto* str = static_cast<std::string*>(data);
        if (len <= 0) {
            str->clear();
            *buf = nullptr;
            return true;
        }
        str->resize(static_cast<size_t>(len - 1));
        *buf = &(*str)[0];
        return true;
    };
    AParcel_readString(in, &result, alloc);
    return result;
}

static int64_t monotonicSec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec);
}

// ============================================================================
// Sysfs Helpers
// ============================================================================

static bool readSysfsStr(const char* path, std::string& out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    out.assign(buf, static_cast<size_t>(n));
    return true;
}

static bool readSysfsInt(const char* path, int64_t& out) {
    std::string s;
    if (!readSysfsStr(path, s) || s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    out = static_cast<int64_t>(v);
    return true;
}

static bool writeSysfsStr(const char* path, const std::string& val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("open(%s, O_WRONLY) failed: %s", path, strerror(errno));
        return false;
    }
    ssize_t n = write(fd, val.c_str(), val.size());
    int savedErr = errno;
    close(fd);
    if (n != static_cast<ssize_t>(val.size())) {
        ALOGW("write(%s, '%s') failed: %s", path, val.c_str(), strerror(savedErr));
        return false;
    }
    return true;
}

static bool writeSysfsInt(const char* path, int64_t val) {
    return writeSysfsStr(path, std::to_string(val));
}

// ============================================================================
// Hardware Telemetry Snapshot
// ============================================================================

struct HwSnapshot {
    int soc = 50;                // % (0..100)
    int tempDeciC = 300;         // 0.1 C (e.g. 350 = 35.0 C)
    int vbatMv = 3900;           // mV
    int ibatMa = 0;              // mA
    int fccMah = 5500;           // mAh
    int designMah = 5500;        // mAh
    int rmMah = 2750;            // mAh
    int cycleCount = 0;
    int sohPct = 100;
    int authentic = 1;
    int timeToFullSec = 0;
    std::string battStatus = "Discharging";

    bool usbOnline = false;
    std::string usbType;
    std::string usbRealType;
    int quickChargeType = 0;     // 0=normal, 1=fast, 2=flash, 3=super, 4=hyper
    int powerMaxW = 0;           // watts
    int vbusMv = 0;              // mV
    int ibusMa = 0;              // mA
    int ccOrientation = 0;
    int otgOnline = 0;

    // Derived ColorOS UI fields
    bool isPcUsb = false;
    bool isWallAc = false;
    int uiIconType = 0;          // 0=normal, 2=VOOC, 3=SuperVOOC
    int uiPower = 0;             // watts shown on lockscreen (e.g. 33)
    int cpaPower = 0;
    int fastCharge = 0;          // 0 or 1
    int isSupportSpeedCharge = 0;// 1 -> enables lockscreen "Hold to speed up"
};

// ============================================================================
// Thermal Safety State Machine (pure function for easy unit testing!)
// ============================================================================

// Tiers:
//   0: < 40.0 C (recover < 38.5 C)
//   1: >= 40.0 C (recover < 38.5 C) -> warm taper for boost (4500 mA)
//   2: >= 42.0 C (recover < 40.5 C) -> 2500 mA
//   3: >= 43.0 C (recover < 41.5 C) -> 1800 mA
//   4: >= 44.0 C (recover < 42.5 C) -> 1500 mA
//   5: >= 45.0 C (recover < 43.5 C) ->  900 mA
static int updateThermalTier(int curTier, int tempDeciC) {
    // Immediate step-up on crossing upper threshold
    int upTier = 0;
    if (tempDeciC >= 450) upTier = 5;
    else if (tempDeciC >= 440) upTier = 4;
    else if (tempDeciC >= 430) upTier = 3;
    else if (tempDeciC >= 420) upTier = 2;
    else if (tempDeciC >= 400) upTier = 1;
    if (upTier > curTier) return upTier;

    // Hysteresis step-down (1.5 C = 15 decidegrees below entry threshold)
    int tier = curTier;
    if (tier >= 5 && tempDeciC < 435) tier = 4;
    if (tier >= 4 && tempDeciC < 425) tier = 3;
    if (tier >= 3 && tempDeciC < 415) tier = 2;
    if (tier >= 2 && tempDeciC < 405) tier = 1;
    if (tier >= 1 && tempDeciC < 385) tier = 0;
    return tier;
}

static int computeThermalLevel(int thermalTier, bool boostActive, bool slowChgActive) {
    int baseLevel = boostActive ? kLevelBoostMax : kLevelNormalPeak;
    if (slowChgActive && !boostActive) {
        baseLevel = std::max(baseLevel, kLevelThermal43); // ~1800 mA when slow charge requested
    }
    int thermalFloor = 0;
    switch (thermalTier) {
        case 0: thermalFloor = 0; break;
        case 1: thermalFloor = boostActive ? kLevelBoostWarm40 : 0; break;
        case 2: thermalFloor = kLevelThermal42; break;
        case 3: thermalFloor = kLevelThermal43; break;
        case 4: thermalFloor = kLevelThermal44; break;
        default: thermalFloor = kLevelThermal45; break;
    }
    return std::max(baseLevel, thermalFloor);
}

// ============================================================================
// Charger Controller State
// ============================================================================

struct ChargerState {
    std::mutex mu;
    std::condition_variable cv;
    bool stopSupervisor = false;
    bool dryRun = false; // true during --self-test so sysfs is never mutated

    // Policy flags (from ColorOS Battery Settings & Lockscreen)
    bool bypassRequested = false;
    bool limitEnabled = false;
    int  limitPct = 80;
    bool mmiChgEnabled = true;
    bool slowChgEnabled = false;
    bool screenOn = true;
    int  otgSwitch = 0;

    // Lockscreen "Hold to speed up" boost window (mode=4) vs Settings Smart Rapid Charging (mode=3)
    bool boostActive = false;
    bool settingsSmartChg = false;
    int64_t boostEndMonoSec = 0;
    int smartChgMode = 0;

    // Thermal & Hardware applied state tracking
    int thermalTier = 0;
    int lastAppliedConstLevel = -1;
    int lastAppliedThermalLimit = -1;
    uint32_t lastAppliedSmartChg = 0xFFFFFFFFu;
    int lastAppliedNightChg = -1;
    bool wasUsbOnline = false;
};

static ChargerState gState;

static HwSnapshot readHwSnapshot(bool bypassActive) {
    HwSnapshot s;
    int64_t v = 0;

    if (readSysfsInt(kSysBattCapacity, v) || readSysfsInt(kSysBmsCapacity, v)) {
        s.soc = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, v)));
    }
    if (readSysfsInt(kSysBattTemp, v) || readSysfsInt(kSysBmsTemp, v)) {
        s.tempDeciC = static_cast<int>(v);
    }
    if (readSysfsInt(kSysBattVolt, v) || readSysfsInt(kSysBmsVolt, v)) {
        s.vbatMv = (v > 100000) ? static_cast<int>(v / 1000) : static_cast<int>(v);
    }
    if (readSysfsInt(kSysBattCurr, v) || readSysfsInt(kSysBmsCurr, v)) {
        s.ibatMa = (std::abs(v) > 20000) ? static_cast<int>(v / 1000) : static_cast<int>(v);
    }
    if (readSysfsInt(kSysBattChargeFull, v) || readSysfsInt(kSysBmsChargeFull, v)) {
        if (v > 100000) s.fccMah = static_cast<int>(v / 1000);
        else if (v > 500) s.fccMah = static_cast<int>(v);
    }
    if (readSysfsInt(kSysBattDesignFull, v) && v >= 2000000) {
        s.designMah = static_cast<int>(v / 1000);
    } else if (readSysfsInt(kSysBmsDesignFull, v) && v >= 2000000) {
        s.designMah = static_cast<int>(v / 1000);
    }
    if (readSysfsInt(kSysBattChargeCnt, v) || readSysfsInt(kSysBmsChargeCnt, v)) {
        if (v > 100000) s.rmMah = static_cast<int>(v / 1000);
        else if (v > 0) s.rmMah = static_cast<int>(v);
    } else {
        s.rmMah = (s.fccMah * s.soc) / 100;
    }
    if (readSysfsInt(kSysBattCycleCount, v) || readSysfsInt(kSysBmsCycleCount, v)) {
        s.cycleCount = static_cast<int>(std::max<int64_t>(0, v));
    }
    if (readSysfsInt(kSysBmsSoh, v) && v >= 50 && v <= 100) {
        s.sohPct = static_cast<int>(v);
    } else if (s.designMah > 0 && s.fccMah > 0) {
        int est = (s.fccMah * 100) / s.designMah;
        s.sohPct = std::max(70, std::min(100, est));
    }
    if (readSysfsInt(kSysBmsAuthentic, v)) {
        s.authentic = (v != 0) ? 1 : 0;
    }
    if (readSysfsInt(kSysBmsTimeToFull, v) && v > 0 && v < 43200) {
        s.timeToFullSec = static_cast<int>(v);
    }
    readSysfsStr(kSysBattStatus, s.battStatus);

    if (readSysfsInt(kSysUsbOnline, v)) {
        s.usbOnline = (v != 0);
    }
    readSysfsStr(kSysUsbType, s.usbType);
    readSysfsStr(kSysUsbRealType, s.usbRealType);
    if (readSysfsInt(kSysUsbQuickType, v)) {
        s.quickChargeType = static_cast<int>(v);
    }
    if (readSysfsInt(kSysUsbPowerMax, v)) {
        s.powerMaxW = static_cast<int>(std::max<int64_t>(0, v));
    }
    if (readSysfsInt(kSysUsbVolt, v)) {
        s.vbusMv = (v > 100000) ? static_cast<int>(v / 1000) : static_cast<int>(v);
    }
    if (readSysfsInt(kSysUsbInputCurr, v) || readSysfsInt(kSysUsbCurr, v)) {
        s.ibusMa = (std::abs(v) > 20000) ? static_cast<int>(v / 1000) : static_cast<int>(v);
    }
    if (readSysfsInt(kSysUsbCcOrient, v)) {
        s.ccOrientation = static_cast<int>(v);
    }
    if (readSysfsInt(kSysUsbOtg, v)) {
        s.otgOnline = (v != 0) ? 1 : 0;
    }

    // Derive ColorOS VOOC / SuperVOOC + wattage + "Hold to speed up" state
    if (s.usbOnline) {
        const std::string& rt = s.usbRealType.empty() ? s.usbType : s.usbRealType;
        bool hasPdOrHv = (rt.find("PD") != std::string::npos ||
                          rt.find("HVDCP") != std::string::npos ||
                          rt.find("PPS") != std::string::npos ||
                          rt.find("QC") != std::string::npos);
        bool isDcpOrCdp = (rt.find("DCP") != std::string::npos ||
                           rt.find("CDP") != std::string::npos ||
                           rt.find("APPLE") != std::string::npos ||
                           rt.find("FLOAT") != std::string::npos);
        bool isSdp = (rt == "USB" || rt == "USB_SDP");

        s.isPcUsb = isSdp && !hasPdOrHv && s.quickChargeType == 0 && s.powerMaxW < 10;
        s.isWallAc = !s.isPcUsb;

        if (hasPdOrHv || s.quickChargeType >= 1 || s.powerMaxW >= 15) {
            // Fast / PD / HVDCP / Xiaomi 33W Turbo -> SuperVOOC!
            s.uiIconType = 3; // SuperVOOC
            int w = (s.powerMaxW >= 15) ? std::min(s.powerMaxW, kDeviceMaxWattage) : kDeviceMaxWattage;
            // Snap 27-33W adapters cleanly to 33W SuperVOOC badge
            if (w >= 27) w = kDeviceMaxWattage;
            s.uiPower = w;
            s.cpaPower = w;
            s.fastCharge = bypassActive ? 0 : 1;
            s.isSupportSpeedCharge = bypassActive ? 0 : 1;
        } else if (isDcpOrCdp || s.powerMaxW >= 10) {
            // Standard wall DCP/CDP adapter -> VOOC!
            s.uiIconType = 2; // VOOC
            int w = (s.powerMaxW >= 10) ? std::min(s.powerMaxW, kDeviceMaxWattage) : 18;
            s.uiPower = w;
            s.cpaPower = w;
            s.fastCharge = bypassActive ? 0 : 1;
            s.isSupportSpeedCharge = bypassActive ? 0 : 1;
        } else {
            // PC USB (500 mA SDP) -> normal slow USB charging
            s.uiIconType = 0;
            s.uiPower = 0;
            s.cpaPower = 0;
            s.fastCharge = 0;
            s.isSupportSpeedCharge = 0;
        }
    }

    return s;
}

// Apply all desired hardware controls (smart_chg, night_charging, constant_charge_current_max,
// charge_control_limit). Called under gState.mu lock.
static void applyHardwareLocked(const HwSnapshot& hw, int64_t nowSec) {
    // 1. Auto-cancel boost on unplug, timer expiry, or bypass enable
    if (gState.boostActive) {
        if (!gState.dryRun && !hw.usbOnline) {
            BRIDGE_LOG_BOOT("Speed-up boost auto-cancelled (charger unplugged)");
            gState.boostActive = false;
            gState.boostEndMonoSec = 0;
            gState.smartChgMode = 0;
        } else if (gState.bypassRequested) {
            BRIDGE_LOG_BOOT("Speed-up boost auto-cancelled (bypass charging enabled)");
            gState.boostActive = false;
            gState.boostEndMonoSec = 0;
            gState.smartChgMode = 0;
        } else if (nowSec >= gState.boostEndMonoSec) {
            BRIDGE_LOG_BOOT("Speed-up boost finished (10-minute window elapsed) -> returning to normal 3000mA cap");
            gState.boostActive = false;
            gState.boostEndMonoSec = 0;
            gState.smartChgMode = 0;
        }
    }

    // Detect plug state transition for clean re-application
    if (hw.usbOnline != gState.wasUsbOnline) {
        gState.wasUsbOnline = hw.usbOnline;
        gState.lastAppliedConstLevel = -1;
        gState.lastAppliedThermalLimit = -1;
        ALOGI("USB online changed -> %d (type=%s real=%s qc=%d power=%dW)",
              hw.usbOnline ? 1 : 0, hw.usbType.c_str(), hw.usbRealType.c_str(),
              hw.quickChargeType, hw.powerMaxW);
    }

    // 2. Compute desired smart_chg + night_charging for Bypass / Limit / MMI
    uint32_t wantSmartChg = kSmartChgDisable;
    int wantNightChg = 0;
    const char* modeDesc = "normal";

    if (gState.bypassRequested || !gState.mmiChgEnabled) {
        // TRUE hardware bypass: smart_chg ENDURANCE target_soc=1 -> ENDURANCE_VOTER=0 mA
        // (keeps VBUS->VSYS power path ON while battery ICHG=0 mA!).
        // Also engage night_charging=1 when SOC>=80% to physically open charge FET.
        wantSmartChg = kSmartChgBypassOn;
        wantNightChg = (hw.soc >= 80) ? 1 : 0;
        modeDesc = gState.bypassRequested ? "BYPASS" : "MMI_PAUSE";
    } else if (gState.limitEnabled) {
        int clamped = std::max(50, std::min(100, gState.limitPct));
        wantSmartChg = packSmartChgEndurance(true, clamped);
        wantNightChg = (clamped == 80) ? 1 : 0;
        modeDesc = "LIMIT";
    }

    if (!gState.dryRun) {
        if (wantSmartChg != gState.lastAppliedSmartChg) {
            if (writeSysfsInt(kSysBattSmartChg, static_cast<int64_t>(wantSmartChg))) {
                gState.lastAppliedSmartChg = wantSmartChg;
                BRIDGE_LOG_BOOT("Applied smart_chg=0x%08x (%u) [%s, limit=%d%%, soc=%d%%]",
                                wantSmartChg, wantSmartChg, modeDesc, gState.limitPct, hw.soc);
            }
        }
        if (wantNightChg != gState.lastAppliedNightChg) {
            if (writeSysfsInt(kSysBattNightChg, wantNightChg)) {
                gState.lastAppliedNightChg = wantNightChg;
                ALOGI("Applied night_charging=%d [%s]", wantNightChg, modeDesc);
            }
        }
    } else {
        gState.lastAppliedSmartChg = wantSmartChg;
        gState.lastAppliedNightChg = wantNightChg;
    }

    // 3. Compute thermal tier (with 1.5 C recovery hysteresis) and target current level
    int prevTier = gState.thermalTier;
    gState.thermalTier = updateThermalTier(gState.thermalTier, hw.tempDeciC);
    int targetLevel = computeThermalLevel(gState.thermalTier, gState.boostActive, gState.slowChgEnabled);

    if (gState.thermalTier != prevTier) {
        BRIDGE_LOG_BOOT("Thermal guard tier %d -> %d (temp=%.1f C, boost=%d -> pd_level=%d)",
                        prevTier, gState.thermalTier, hw.tempDeciC / 10.0,
                        gState.boostActive ? 1 : 0, targetLevel);
    }

    if (!gState.dryRun && hw.usbOnline && !gState.bypassRequested && gState.mmiChgEnabled) {
        // Check actual kernel level so we automatically re-apply after tanzanite's 60s boot quirk!
        int64_t curKernelConst = -1;
        readSysfsInt(kSysBattConstCurr, curKernelConst);
        if (curKernelConst != targetLevel || gState.lastAppliedConstLevel != targetLevel) {
            if (writeSysfsInt(kSysBattConstCurr, targetLevel)) {
                gState.lastAppliedConstLevel = targetLevel;
                ALOGI("Set constant_charge_current_max=%d (boost=%d tier=%d temp=%.1fC ibat=%dmA)",
                      targetLevel, gState.boostActive ? 1 : 0, gState.thermalTier,
                      hw.tempDeciC / 10.0, hw.ibatMa);
            }
        }

        // Override charge_control_limit (TEMP_THERMAL_DAEMON_VOTER) during:
        //   (a) Active 10-min boost (so full ~5.6 A isn't prematurely capped while cool < 42 C)
        //   (b) Thermal safety tier >= 2 (>= 42 C, guaranteeing safe battery cool-down!)
        if (gState.boostActive || gState.thermalTier >= 2) {
            int64_t curKernelLim = -1;
            readSysfsInt(kSysBattThermalLim, curKernelLim);
            if (curKernelLim != targetLevel || gState.lastAppliedThermalLimit != targetLevel) {
                if (writeSysfsInt(kSysBattThermalLim, targetLevel)) {
                    gState.lastAppliedThermalLimit = targetLevel;
                }
            }
        } else {
            gState.lastAppliedThermalLimit = -1;
        }
    } else if (gState.dryRun) {
        gState.lastAppliedConstLevel = targetLevel;
        gState.lastAppliedThermalLimit = (gState.boostActive || gState.thermalTier >= 2) ? targetLevel : -1;
    }
}

static void kickSupervisorLocked() {
    gState.cv.notify_all();
}

static void supervisorThreadMain() {
    BRIDGE_LOG_BOOT("Charger supervisor thread started (1s thermal/boost/bypass control loop)");
    std::unique_lock<std::mutex> lock(gState.mu);
    while (!gState.stopSupervisor) {
        bool bypass = gState.bypassRequested;
        lock.unlock();
        HwSnapshot hw = readHwSnapshot(bypass);
        int64_t now = monotonicSec();
        lock.lock();
        if (gState.stopSupervisor) break;
        applyHardwareLocked(hw, now);
        auto sleepMs = gState.boostActive ? std::chrono::milliseconds(300)
                                          : std::chrono::milliseconds(1000);
        gState.cv.wait_for(lock, sleepMs, [] { return gState.stopSupervisor; });
    }
    BRIDGE_LOG_BOOT("Charger supervisor thread exiting");
}

// ============================================================================
// ColorOS ChgConfig Parsers & JSON Builder
// ============================================================================

// Parse bypass string: "1+switch=1", "1+switch=0", "switch=1", "1", "0"
static bool parseBypassString(const std::string& extra, bool& outEnable) {
    size_t pos = extra.find("switch=");
    if (pos != std::string::npos && pos + 7 < extra.size()) {
        outEnable = (extra[pos + 7] == '1');
        return true;
    }
    if (extra == "1" || extra == "true") {
        outEnable = true;
        return true;
    }
    if (extra == "0" || extra == "false") {
        outEnable = false;
        return true;
    }
    return false;
}

// Parse charging limit strings from Battery.apk:
//   flag=14 (SET_CHG_UP_LIMIT_STATE): "2++<en>+<limit>"        e.g. "2++1+80", "2++0+80"
//   flag=5  (SET_CHG_UP_LIMIT_VALUE): "4++<en>+<limit>+..."    e.g. "4++1+80+1+80", "4++0+80+1+80"
static bool parseLimitString(const std::string& extra, bool& outEnable, int& outLimitPct) {
    // Split on '+' ignoring empty tokens
    std::vector<int> nums;
    size_t i = 0;
    while (i < extra.size()) {
        while (i < extra.size() && (extra[i] == '+' || extra[i] == ',' || extra[i] == ' ')) ++i;
        if (i >= extra.size()) break;
        size_t j = i;
        while (j < extra.size() && extra[j] != '+' && extra[j] != ',' && extra[j] != ' ') ++j;
        char* end = nullptr;
        long v = strtol(extra.substr(i, j - i).c_str(), &end, 10);
        if (end && *end == '\0') {
            nums.push_back(static_cast<int>(v));
        }
        i = j;
    }
    // Expected: [cmd(2 or 4), en(0/1), limit(50..100), ...]
    if (nums.size() >= 3 && (nums[0] == 2 || nums[0] == 4)) {
        outEnable = (nums[1] != 0);
        if (nums[2] >= 50 && nums[2] <= 100) {
            outLimitPct = nums[2];
        }
        return true;
    }
    // Fallback: [en, limit]
    if (nums.size() == 2 && (nums[0] == 0 || nums[0] == 1) && nums[1] >= 50 && nums[1] <= 100) {
        outEnable = (nums[0] != 0);
        outLimitPct = nums[1];
        return true;
    }
    return false;
}

static std::string buildChargerInfoUpdateJson(const HwSnapshot& hw, bool bypassActive) {
    int bypassStatus = (bypassActive && hw.usbOnline) ? 1 : 0;
    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"Update\":{"
             "\"uiIconType\":%d,"
             "\"uiPower\":%d,"
             "\"cpaPower\":%d,"
             "\"fastCharge\":%d,"
             "\"isSupportSpeedCharge\":%d,"
             "\"bypassChgStatus\":%d,"
             "\"notifyCode\":0,"
             "\"batteryFcc\":%d,"
             "\"batteryRm\":%d,"
             "\"usbStatus\":0,"
             "\"otgOnline\":%d,"
             "\"ppsCharging\":0,"
             "\"ppsChargePower\":0,"
             "\"shortCHwStatus\":0,"
             "\"shortIcOtpStatus\":0,"
             "\"bmsStatus\":0,"
             "\"enableTx\":\"0\","
             "\"wlsAdapterPower\":0,"
             "\"powerbankStatus\":0,"
             "\"wlsPowerbankStatus\":0,"
             "\"chargeTimeRemaining\":%d"
             "}}",
             hw.uiIconType,
             hw.uiPower,
             hw.cpaPower,
             hw.fastCharge,
             hw.isSupportSpeedCharge,
             bypassStatus,
             hw.fccMah,
             hw.rmMah,
             hw.otgOnline,
             hw.timeToFullSec);
    return std::string(buf);
}

static std::string handleGetChgConfig(int32_t flag, const std::string& extra, int32_t param) {
    bool bypassRequested = false;
    {
        std::lock_guard<std::mutex> lock(gState.mu);
        bypassRequested = gState.bypassRequested;
    }
    HwSnapshot hw = readHwSnapshot(bypassRequested);
    bool bypassActive = bypassRequested && hw.usbOnline;

    switch (flag) {
        case FLAG_GET_UI_CHG_ICON_TYPE:
            return std::to_string(hw.uiIconType);
        case FLAG_GET_UI_POWER_VALUE:
            return std::to_string(hw.uiPower);
        case FLAG_GET_DEVICE_POWER_VALUE:
            return std::to_string(kDeviceMaxWattage);
        case FLAG_GET_ADAPTER_POWER_VALUE:
            return std::to_string(hw.usbOnline ? (hw.powerMaxW > 0 ? hw.powerMaxW : hw.uiPower) : 0);
        case FLAG_GET_CPA_POWER_VALUE:
            return std::to_string(hw.cpaPower);
        case FLAG_IS_SUPPORT_SINGLE_HYPER_SPEED:
            return std::to_string(hw.isSupportSpeedCharge);
        case FLAG_IS_SUPPORT_ECO_DESIGN:
            return "1"; // Enables ColorOS Charging Limit & Battery Health features
        case FLAG_GET_BATT_MANU_DATE:
            return "20250101";
        case FLAG_GET_BATT_FIRST_USAGE_DATE:
            return "20250101";
        case FLAG_GET_BATT_UI_CYCLE_COUNT:
            return std::to_string(hw.cycleCount);
        case FLAG_GET_BATTERY_TYPE:
            return "Li-poly";
        case FLAG_GET_BATTERY_GAUGE_TYPE:
            return "1";
        case FLAG_GET_BATTERY_STATUS_PM_VPH:
        case FLAG_GET_BATTERY_STATUS_PM_VBAT:
            return std::to_string(hw.vbatMv);
        case FLAG_GET_CHG_BYPASS_STATUS:
            return bypassActive ? "1" : "0";
        case FLAG_GET_CHARGER_INFO:
        case FLAG_GET_CHARGER_INFO_UPDATE:
            return buildChargerInfoUpdateJson(hw, bypassActive);
        case FLAG_CHECK_FLAG_SUPPORT: {
            // ColorOS OplusBatteryService convention (verified in oplus-services.jar):
            //   "0"  = supported (POSIX OK -> calls getDataFromHalByUpdate())
            //   "-1" = unsupported (falls back to old interface)
            int qFlag = -1;
            if (!extra.empty()) {
                qFlag = atoi(extra.c_str());
            } else {
                qFlag = param;
            }
            switch (qFlag) {
                case FLAG_GET_UI_CHG_ICON_TYPE:
                case FLAG_GET_UI_POWER_VALUE:
                case FLAG_GET_DEVICE_POWER_VALUE:
                case FLAG_GET_ADAPTER_POWER_VALUE:
                case FLAG_GET_CPA_POWER_VALUE:
                case FLAG_SET_CHG_UP_LIMIT_VALUE:
                case FLAG_IS_SUPPORT_SINGLE_HYPER_SPEED:
                case FLAG_IS_SUPPORT_ECO_DESIGN:
                case FLAG_SET_RECHG_SOC_EN:
                case FLAG_SET_CHG_UP_LIMIT_STATE:
                case FLAG_GET_BATT_UI_CYCLE_COUNT:
                case FLAG_GET_CHG_BYPASS_STATUS:
                case FLAG_SET_CHG_BYPASS:
                case FLAG_GET_CHARGER_INFO:
                case FLAG_GET_CHARGER_INFO_UPDATE:
                case FLAG_CHECK_FLAG_SUPPORT:
                    return "0";
                default:
                    return "-1";
            }
        }
        default:
            ALOGI("getChgConfig(flag=%d, extra='%s', param=%d) -> \"0\"", flag, extra.c_str(), param);
            return "0";
    }
}

static int32_t handleSetChgConfig(int32_t flag, const std::string& extra, int32_t param) {
    ALOGI("setChgConfig(flag=%d, extra='%s', param=%d)", flag, extra.c_str(), param);
    switch (flag) {
        case FLAG_SET_CHG_BYPASS: {
            bool enable = false;
            if (parseBypassString(extra, enable)) {
                HwSnapshot hw = readHwSnapshot(enable);
                int64_t now = monotonicSec();
                std::lock_guard<std::mutex> lock(gState.mu);
                bool changed = (gState.bypassRequested != enable);
                gState.bypassRequested = enable;
                if (changed) {
                    BRIDGE_LOG_BOOT("Bypass Charging %s (extra='%s', soc=%d%%, usb=%d)",
                                    enable ? "ENABLED" : "DISABLED",
                                    extra.c_str(), hw.soc, hw.usbOnline ? 1 : 0);
                }
                applyHardwareLocked(hw, now);
                kickSupervisorLocked();
            } else {
                ALOGW("setChgConfig(SET_CHG_BYPASS): unparsed extra='%s'", extra.c_str());
            }
            return 0;
        }
        case FLAG_SET_CHG_UP_LIMIT_STATE:
        case FLAG_SET_CHG_UP_LIMIT_VALUE: {
            bool enable = false;
            int pct = 80;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                pct = gState.limitPct;
            }
            if (parseLimitString(extra, enable, pct)) {
                HwSnapshot hw = readHwSnapshot(false);
                int64_t now = monotonicSec();
                std::lock_guard<std::mutex> lock(gState.mu);
                bool changed = (gState.limitEnabled != enable || gState.limitPct != pct);
                gState.limitEnabled = enable;
                gState.limitPct = pct;
                if (changed) {
                    BRIDGE_LOG_BOOT("Charging Limit %s at %d%% (flag=%d, extra='%s')",
                                    enable ? "ENABLED" : "DISABLED", pct, flag, extra.c_str());
                }
                applyHardwareLocked(hw, now);
                kickSupervisorLocked();
            } else {
                ALOGW("setChgConfig(flag=%d): unparsed limit extra='%s'", flag, extra.c_str());
            }
            return 0;
        }
        case FLAG_SET_RECHG_SOC_EN: {
            ALOGI("setChgConfig(SET_RECHG_SOC_EN): extra='%s' param=%d", extra.c_str(), param);
            return 0;
        }
        default:
            return 0;
    }
}

static int32_t handleSetCustomSelectChgMode(int32_t mode, bool enable) {
    HwSnapshot hw = readHwSnapshot(false);
    int64_t now = monotonicSec();
    std::lock_guard<std::mutex> lock(gState.mu);
    // ColorOS 17 SmartChargeController (Battery.apk Laa/a$f):
    //   mode=4, enable=true  -> Lockscreen "Hold to speed up" (single_speed_charge_state=1)
    //   mode=4, enable=false -> Stop lockscreen "Hold to speed up"
    //   mode=3, enable=t/f   -> Settings "Smart Rapid Charging" toggle (keeps normal <=3000mA cap)
    //   mode=0, enable=false -> Generic speed-charge disable
    if (enable && mode == 4) {
        if (gState.bypassRequested) {
            BRIDGE_LOG_BOOT("Lockscreen 'Hold to speed up' (mode=4) ignored while Bypass Charging is ON");
            return 0;
        }
        gState.boostActive = true;
        gState.boostEndMonoSec = now + kBoostDurationSec;
        gState.smartChgMode = 4;
        BRIDGE_LOG_BOOT("Lockscreen 'Hold to speed up' ACTIVATED (mode=4)! Boosting up to ~5.6A for %llds (soc=%d%%, temp=%.1f C)",
                        static_cast<long long>(kBoostDurationSec), hw.soc, hw.tempDeciC / 10.0);
    } else if (!enable && (mode == 4 || mode == 0)) {
        if (gState.boostActive) {
            BRIDGE_LOG_BOOT("Lockscreen 'Hold to speed up' STOPPED (mode=%d, enable=0) -> returning to normal 3000mA cap",
                            mode);
        }
        gState.boostActive = false;
        gState.boostEndMonoSec = 0;
        gState.smartChgMode = gState.settingsSmartChg ? 3 : 0;
    } else if (mode == 3) {
        gState.settingsSmartChg = enable;
        if (!gState.boostActive) {
            gState.smartChgMode = enable ? 3 : 0;
        }
        ALOGI("Settings Smart Rapid Charging (mode=3, enable=%d) -> normal <=3000mA cap preserved until lockscreen hold",
              enable ? 1 : 0);
    } else {
        gState.smartChgMode = enable ? mode : 0;
        ALOGI("setCustomSelectChgMode(mode=%d, enable=%d)", mode, enable ? 1 : 0);
    }
    applyHardwareLocked(hw, now);
    kickSupervisorLocked();
    return 0;
}

// ============================================================================
// AIDL Binder Transaction Dispatcher (Full ICharger V12)
// ============================================================================

static void* onCreateCharger(void* arg) { return arg; }
static void onDestroyCharger(void*) {}

static binder_status_t onTransactCharger(
        AIBinder*, transaction_code_t code, const AParcel* in, AParcel* out) {
    switch (code) {
        case TX_getInterfaceVersion:
            writeOkInt(out, kIChargerVersion);
            return STATUS_OK;
        case TX_getInterfaceHash:
            writeOkStr(out, kIChargerHash);
            return STATUS_OK;

        // ---- Core V12 Config & JSON Telemetry ----
        case TX_getChgConfig: {
            int32_t flag = 0, param = 0;
            AParcel_readInt32(in, &flag);
            std::string extra = readStr(in);
            AParcel_readInt32(in, &param);
            std::string res = handleGetChgConfig(flag, extra, param);
            ALOGI("getChgConfig(flag=%d, extra='%s', param=%d) -> '%s'",
                  flag, extra.c_str(), param, res.c_str());
            writeOkStr(out, res);
            return STATUS_OK;
        }
        case TX_setChgConfig: {
            int32_t flag = 0, param = 0;
            AParcel_readInt32(in, &flag);
            std::string extra = readStr(in);
            AParcel_readInt32(in, &param);
            int32_t ret = handleSetChgConfig(flag, extra, param);
            writeOkInt(out, ret);
            return STATUS_OK;
        }
        case TX_queryChargeInfo: {
            bool bypass = false;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                bypass = gState.bypassRequested;
            }
            HwSnapshot hw = readHwSnapshot(bypass);
            writeOkStr(out, buildChargerInfoUpdateJson(hw, bypass && hw.usbOnline));
            return STATUS_OK;
        }

        // ---- Lockscreen "Hold to speed up" & Smart Charging Mode ----
        case TX_setCustomSelectChgMode: {
            int32_t mode = 0;
            bool enable = false;
            AParcel_readInt32(in, &mode);
            AParcel_readBool(in, &enable);
            int32_t ret = handleSetCustomSelectChgMode(mode, enable);
            writeOkInt(out, ret);
            return STATUS_OK;
        }
        case TX_getCustomSelectChgMode: {
            int32_t mode = 0;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                mode = gState.boostActive ? 4 : gState.smartChgMode;
            }
            writeOkInt(out, mode);
            return STATUS_OK;
        }
        case TX_getSmartChgMode: {
            int32_t mode = 0;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                mode = gState.boostActive ? 4 : gState.smartChgMode;
            }
            writeOkInt(out, mode);
            return STATUS_OK;
        }
        case TX_setSmartChgMode: {
            std::string val = readStr(in);
            ALOGI("setSmartChgMode('%s')", val.c_str());
            int m = atoi(val.c_str());
            if (m == 4) {
                handleSetCustomSelectChgMode(4, true);
            } else if (m == 0) {
                handleSetCustomSelectChgMode(0, false);
            }
            writeOkInt(out, 0);
            return STATUS_OK;
        }

        // ---- Legacy Charging Pause / Slow Charge / Screen / CoolDown ----
        case TX_setPsyMmiChgEn: {
            std::string val = readStr(in);
            bool enable = (val != "0" && val != "false");
            HwSnapshot hw = readHwSnapshot(!enable);
            int64_t now = monotonicSec();
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                if (gState.mmiChgEnabled != enable) {
                    BRIDGE_LOG_BOOT("setPsyMmiChgEn('%s') -> charging %s",
                                    val.c_str(), enable ? "RESUMED" : "PAUSED");
                }
                gState.mmiChgEnabled = enable;
                applyHardwareLocked(hw, now);
                kickSupervisorLocked();
            }
            writeOkInt(out, 0);
            return STATUS_OK;
        }
        case TX_setPsySlowChgEn: {
            std::string val = readStr(in);
            bool slow = (!val.empty() && val[0] == '1');
            HwSnapshot hw = readHwSnapshot(false);
            int64_t now = monotonicSec();
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                gState.slowChgEnabled = slow;
                applyHardwareLocked(hw, now);
                kickSupervisorLocked();
            }
            ALOGI("setPsySlowChgEn('%s') -> slow=%d", val.c_str(), slow ? 1 : 0);
            writeOkInt(out, 0);
            return STATUS_OK;
        }
        case TX_notifyScreenStatus: {
            int32_t status = 1;
            AParcel_readInt32(in, &status);
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                gState.screenOn = (status != 0);
            }
            writeOkInt(out, 0);
            return STATUS_OK;
        }
        case TX_getChargerCoolDown: {
            int lvl = 0;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                lvl = (gState.lastAppliedConstLevel >= 0) ? gState.lastAppliedConstLevel : kLevelNormalPeak;
            }
            writeOkInt(out, lvl);
            return STATUS_OK;
        }
        case TX_setChargerCoolDown: {
            std::string s = readStr(in);
            ALOGI("setChargerCoolDown('%s') (managed by thermal/boost supervisor)", s.c_str());
            writeOkInt(out, 0);
            return STATUS_OK;
        }
        case TX_setSmartCoolDown: {
            int32_t a = 0, b = 0;
            AParcel_readInt32(in, &a);
            AParcel_readInt32(in, &b);
            std::string c = readStr(in);
            ALOGI("setSmartCoolDown(%d, %d, '%s') (managed by thermal/boost supervisor)", a, b, c.c_str());
            writeOkInt(out, 0);
            return STATUS_OK;
        }

        // ---- Live Battery / USB / VOOC Telemetry Getters ----
        case TX_getAcType: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.isWallAc ? 1 : 0);
            return STATUS_OK;
        }
        case TX_getBattAuthenticate: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.authentic);
            return STATUS_OK;
        }
        case TX_getBattVoocChgIng:
        case TX_getFastCharge: {
            bool bypass = false;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                bypass = gState.bypassRequested;
            }
            HwSnapshot hw = readHwSnapshot(bypass);
            writeOkInt(out, hw.fastCharge);
            return STATUS_OK;
        }
        case TX_getBatteryVoltageNow: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.vbatMv);
            return STATUS_OK;
        }
        case TX_getPsyAcOnline: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, (hw.usbOnline && hw.isWallAc) ? 1 : 0);
            return STATUS_OK;
        }
        case TX_getPsyBatteryCC: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.cycleCount);
            return STATUS_OK;
        }
        case TX_getPsyBatteryCurrentNow: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.ibatMa);
            return STATUS_OK;
        }
        case TX_getPsyBatteryFcc: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.fccMah);
            return STATUS_OK;
        }
        case TX_getPsyBatteryHmac:
            writeOkInt(out, 1);
            return STATUS_OK;
        case TX_getPsyBatteryLevel: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.soc);
            return STATUS_OK;
        }
        case TX_getPsyBatteryRm: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.rmMah);
            return STATUS_OK;
        }
        case TX_getPsyBatteryStatus: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkStr(out, hw.battStatus);
            return STATUS_OK;
        }
        case TX_getPsyBatteryTemp: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.tempDeciC);
            return STATUS_OK;
        }
        case TX_getPsyChargeTech: {
            bool bypass = false;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                bypass = gState.bypassRequested;
            }
            HwSnapshot hw = readHwSnapshot(bypass);
            writeOkInt(out, hw.uiIconType);
            return STATUS_OK;
        }
        case TX_getPsyFastChgType: {
            bool bypass = false;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                bypass = gState.bypassRequested;
            }
            HwSnapshot hw = readHwSnapshot(bypass);
            // 0 = normal, 1 = VOOC, 2 = SuperVOOC
            int t = (hw.uiIconType == 3) ? 2 : ((hw.uiIconType == 2) ? 1 : 0);
            writeOkInt(out, t);
            return STATUS_OK;
        }
        case TX_getPsyInputCurrent:
        case TX_getUsbInputCurrentNow: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.ibusMa);
            return STATUS_OK;
        }
        case TX_getPsyOtgOnline:
        case TX_getWiredOtgOnline: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.otgOnline);
            return STATUS_OK;
        }
        case TX_getPsyOtgSwitch: {
            int v = 0;
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                v = gState.otgSwitch;
            }
            writeOkInt(out, v);
            return STATUS_OK;
        }
        case TX_setPsyOtgSwitch: {
            std::string val = readStr(in);
            {
                std::lock_guard<std::mutex> lock(gState.mu);
                gState.otgSwitch = (val == "1" || val == "true") ? 1 : 0;
            }
            writeOkInt(out, 0);
            return STATUS_OK;
        }
        case TX_getPsyPcPortOnline: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.isPcUsb ? 1 : 0);
            return STATUS_OK;
        }
        case TX_getPsyTypeOrientation: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.ccOrientation);
            return STATUS_OK;
        }
        case TX_getPsyUsbOnline: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.usbOnline ? 1 : 0);
            return STATUS_OK;
        }
        case TX_getUIsohValue: {
            HwSnapshot hw = readHwSnapshot(false);
            writeOkInt(out, hw.sohPct);
            return STATUS_OK;
        }
        case TX_getDevinfoFastchg:
            writeOkStr(out, "1");
            return STATUS_OK;
        case TX_getPsyBatterySN:
            writeOkStr(out, "TANZANITE0001");
            return STATUS_OK;
        case TX_getQuickModeGain:
            writeOkStr(out, "0,0");
            return STATUS_OK;
        case TX_getBmsHeatingRunningStatus:
        case TX_getChargerControl:
        case TX_getWirelessTXEnable:
        case TX_getWirelessDeviated:
            writeOkStr(out, "0");
            return STATUS_OK;
        case TX_healthd_update_ui_soc_decimal:
            // Empty lets SystemUI BatteryLevelToDecimalHelper interpolate smoothly without format mismatch
            writeOkStr(out, "");
            return STATUS_OK;

        // ---- String-returning optional / wireless / FTM / debug methods ----
        case TX_getBattParamNoplug:
        case TX_getBccCsvData:
        case TX_getPsyWirelessRX:
        case TX_getPsyWirelessRxVersion:
        case TX_getPsyWirelessTX:
        case TX_getPsyWirelessTxVersion:
        case TX_getReserveSocDebug:
        case TX_getUisohDebugParameterInfo:
        case TX_testKitGetFeatureList:
        case TX_queryWlsPencilInfo:
        case TX_getChgOlcConfig:
        case TX_getBattGaugeInfo:
        case TX_queryRgcpHboostInfo:
        case TX_getDeepSleepClustersSleepWake:
            writeOkStr(out, "");
            return STATUS_OK;

        case TX_testKitGetFeatureName:
        case TX_getUsbCurrentEyeDiagram: {
            int32_t idx = 0;
            AParcel_readInt32(in, &idx);
            writeOkStr(out, "");
            return STATUS_OK;
        }
        case TX_setCV: {
            int32_t a = 0, b = 0;
            AParcel_readInt32(in, &a);
            AParcel_readInt32(in, &b);
            writeOkStr(out, "0");
            return STATUS_OK;
        }
        case TX_getPowerbankInfo: {
            (void)readStr(in);
            int32_t a = 0;
            AParcel_readInt32(in, &a);
            writeOkStr(out, "");
            return STATUS_OK;
        }
        case TX_testKitFeatureTest: {
            int32_t idx = 0;
            AParcel_readInt32(in, &idx);
            writeOkStatus(out);
            ParcelableWriter pw(out, true);
            AParcel_writeInt32(out, 0);
            AParcel_writeString(out, "", 0);
            pw.finish();
            return STATUS_OK;
        }

        // ---- Int-returning setters taking (string) ----
        case TX_VolDividerIcWorkModeSet:
        case TX_setChargeEMMode:
        case TX_setChargerControl:
        case TX_setChargerCriticalLog:
        case TX_setChargerCycle:
        case TX_setChargerFactoryModeTest:
        case TX_setChargerLog:
        case TX_setFastchgFwUpdate:
        case TX_setReserveSocDebug:
        case TX_setShipMode:
        case TX_setTbattPwrOff:
        case TX_setUisohDebugInfo:
        case TX_setUsbPrimalType:
        case TX_setWirelessChargePumpEn:
        case TX_setWirelessFtmMode:
        case TX_setWirelessIconDelay:
        case TX_setWirelessIdtAdcTest:
        case TX_setWirelessPenSoc:
        case TX_setWirelessRXEnable:
        case TX_setWirelessTXEnable:
        case TX_setWirelessUserSleepMode:
        case TX_setWlsThirdPartitionInfo:
        case TX_setChgOlcConfig:
        case TX_setSuperEnduranceStatus:
        case TX_setSuperEnduranceCount:
        case TX_setBobStatus:
        case TX_setBatteryLogPush:
        case TX_setChargingModeInGsmCall:
        case TX_setChgRusConfig: {
            (void)readStr(in);
            writeOkInt(out, 0);
            return STATUS_OK;
        }

        // ---- Int-returning setters taking (int) ----
        case TX_nightstandby:
        case TX_setChgStatusToBcc: {
            int32_t a = 0;
            AParcel_readInt32(in, &a);
            writeOkInt(out, 0);
            return STATUS_OK;
        }

        // ---- Int-returning setter taking (int, string, bool) ----
        case TX_setUsbEyeDiagram: {
            int32_t a = 0;
            bool b = false;
            AParcel_readInt32(in, &a);
            (void)readStr(in);
            AParcel_readBool(in, &b);
            writeOkInt(out, 0);
            return STATUS_OK;
        }

        // ---- Remaining no-arg int-returning getters (wireless / diagnostic / PPS / short) ----
        case TX_chgExchangeMesgInit:
        case TX_chgExchangeSohMesgInit:
        case TX_getBattPPSChgIng:
        case TX_getBattPPSChgPower:
        case TX_getBattShortIcOtpStatus:
        case TX_getBattSubCurrent:
        case TX_getBccExpStatus:
        case TX_getBmsHeatingStatus:
        case TX_getChargerCriticalLog:
        case TX_getChargerIdVolt:
        case TX_getChargerLog:
        case TX_getParallelChgMosTestResult:
        case TX_getPsyBatteryNotify:
        case TX_getPsyBatteryPchg:
        case TX_getPsyBatteryPchgResetCount:
        case TX_getPsyBatteryShortFeature:
        case TX_getPsyBatteryShortStatus:
        case TX_getPsyQGVbatDeviation:
        case TX_getPsyUsbStatus:
        case TX_getQgVbatDeviation:
        case TX_getUsbPrimalType:
        case TX_getWirelessAdapterPower:
        case TX_getWirelessCapacity:
        case TX_getWirelessChargePumpEn:
        case TX_getWirelessCurrentNow:
        case TX_getWirelessOnline:
        case TX_getWirelessPenPresent:
        case TX_getWirelessPtmcId:
        case TX_getWirelessRXEnable:
        case TX_getWirelessRealType:
        case TX_getWirelessUserSleepMode:
        case TX_getWirelessVoltageNow:
        case TX_testKitGetFeatureNum:
        case TX_updateUiSohToPartion:
        case TX_getCpVbatDeviation:
        case TX_getChargingModeInGsmCall:
            writeOkInt(out, 0);
            return STATUS_OK;

        default:
            ALOGW("ICharger: unhandled tx=%u", code);
            return STATUS_UNKNOWN_TRANSACTION;
    }
}

static AIBinder_Class* gChargerClass = nullptr;

// ============================================================================
// Comprehensive Self-Test (dry-run, zero sysfs mutation)
// ============================================================================

static bool transactIntOut(AIBinder* b, transaction_code_t tx, int32_t& outVal) {
    AParcel *in = nullptr, *out = nullptr;
    AStatus* st = nullptr;
    AIBinder_prepareTransaction(b, &in);
    binder_status_t bs = AIBinder_transact(b, tx, &in, &out, 0);
    bool ok = false;
    if (bs == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &st);
        ok = (st && AStatus_isOk(st) && AParcel_readInt32(out, &outVal) == STATUS_OK);
    }
    if (st) AStatus_delete(st);
    if (out) AParcel_delete(out);
    return ok;
}

static bool transactStrOut(AIBinder* b, transaction_code_t tx, std::string& outStr) {
    AParcel *in = nullptr, *out = nullptr;
    AStatus* st = nullptr;
    AIBinder_prepareTransaction(b, &in);
    binder_status_t bs = AIBinder_transact(b, tx, &in, &out, 0);
    bool ok = false;
    if (bs == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &st);
        if (st && AStatus_isOk(st)) {
            outStr = readStr(out);
            ok = true;
        }
    }
    if (st) AStatus_delete(st);
    if (out) AParcel_delete(out);
    return ok;
}

static bool transactGetChgConfig(AIBinder* b, int32_t flag, const char* extra, int32_t param, std::string& outStr) {
    AParcel *in = nullptr, *out = nullptr;
    AStatus* st = nullptr;
    AIBinder_prepareTransaction(b, &in);
    AParcel_writeInt32(in, flag);
    AParcel_writeString(in, extra, static_cast<int32_t>(strlen(extra)));
    AParcel_writeInt32(in, param);
    binder_status_t bs = AIBinder_transact(b, TX_getChgConfig, &in, &out, 0);
    bool ok = false;
    if (bs == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &st);
        if (st && AStatus_isOk(st)) {
            outStr = readStr(out);
            ok = true;
        }
    }
    if (st) AStatus_delete(st);
    if (out) AParcel_delete(out);
    return ok;
}

static bool transactSetChgConfig(AIBinder* b, int32_t flag, const char* extra, int32_t param, int32_t& outRet) {
    AParcel *in = nullptr, *out = nullptr;
    AStatus* st = nullptr;
    AIBinder_prepareTransaction(b, &in);
    AParcel_writeInt32(in, flag);
    AParcel_writeString(in, extra, static_cast<int32_t>(strlen(extra)));
    AParcel_writeInt32(in, param);
    binder_status_t bs = AIBinder_transact(b, TX_setChgConfig, &in, &out, 0);
    bool ok = false;
    if (bs == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &st);
        ok = (st && AStatus_isOk(st) && AParcel_readInt32(out, &outRet) == STATUS_OK);
    }
    if (st) AStatus_delete(st);
    if (out) AParcel_delete(out);
    return ok;
}

static bool transactSetCustomMode(AIBinder* b, int32_t mode, bool enable, int32_t& outRet) {
    AParcel *in = nullptr, *out = nullptr;
    AStatus* st = nullptr;
    AIBinder_prepareTransaction(b, &in);
    AParcel_writeInt32(in, mode);
    AParcel_writeBool(in, enable);
    binder_status_t bs = AIBinder_transact(b, TX_setCustomSelectChgMode, &in, &out, 0);
    bool ok = false;
    if (bs == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &st);
        ok = (st && AStatus_isOk(st) && AParcel_readInt32(out, &outRet) == STATUS_OK);
    }
    if (st) AStatus_delete(st);
    if (out) AParcel_delete(out);
    return ok;
}

static int runSelfTest() {
    printf("[ChargerAidlBridge] Running V12 AIDL + logic self-test (dry-run)...\n");
    gState.dryRun = true;

    gChargerClass = AIBinder_Class_define(
            kIChargerDescriptor, onCreateCharger, onDestroyCharger, onTransactCharger);
    AIBinder* b = AIBinder_new(gChargerClass, nullptr);
    AIBinder_markVintfStability(b);

    bool pass = true;
    auto check = [&](const char* name, bool cond) {
        printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
        if (!cond) pass = false;
    };

    // 1. Version & Hash
    int32_t ver = -1;
    std::string hash;
    check("getInterfaceVersion == 12", transactIntOut(b, TX_getInterfaceVersion, ver) && ver == 12);
    check("getInterfaceHash matches V12", transactStrOut(b, TX_getInterfaceHash, hash) && hash == kIChargerHash);

    // 2. Flag support & JSON update
    std::string cfg;
    check("CHECK_FLAG_SUPPORT(30) == 0 (supported)",
          transactGetChgConfig(b, FLAG_CHECK_FLAG_SUPPORT, "30", 0, cfg) && cfg == "0");
    check("CHECK_FLAG_SUPPORT(999) == -1 (unsupported)",
          transactGetChgConfig(b, FLAG_CHECK_FLAG_SUPPORT, "999", 0, cfg) && cfg == "-1");
    check("IS_SUPPORT_ECO_DESIGN == 1",
          transactGetChgConfig(b, FLAG_IS_SUPPORT_ECO_DESIGN, "", 0, cfg) && cfg == "1");
    check("GET_DEVICE_POWER_VALUE == 33",
          transactGetChgConfig(b, FLAG_GET_DEVICE_POWER_VALUE, "", 0, cfg) && cfg == "33");
    check("GET_CHARGER_INFO_UPDATE JSON valid",
          transactGetChgConfig(b, FLAG_GET_CHARGER_INFO_UPDATE, "Update", 1, cfg) &&
          cfg.find("{\"Update\":{") == 0 &&
          cfg.find("\"uiIconType\":") != std::string::npos &&
          cfg.find("\"isSupportSpeedCharge\":") != std::string::npos &&
          cfg.find("\"bypassChgStatus\":") != std::string::npos);
    printf("  Live snapshot JSON: %s\n", cfg.c_str());

    // 3. smart_chg bit packing math
    check("smart_chg BYPASS ON == 65553 (0x00010011)", kSmartChgBypassOn == 65553u);
    check("smart_chg LIMIT 80% == 5242897 (0x00500011)", packSmartChgEndurance(true, 80) == 5242897u);
    check("smart_chg DISABLE == 16 (0x00000010)", kSmartChgDisable == 16u);

    // 4. Charging Limit (flag=14 "2++1+80", flag=5 "4++1+85+1+85", disable "2++0+80")
    int32_t ret = -1;
    check("setChgConfig(14, '2++1+80') -> smart_chg=5242897",
          transactSetChgConfig(b, FLAG_SET_CHG_UP_LIMIT_STATE, "2++1+80", 0, ret) &&
          ret == 0 && gState.limitEnabled && gState.limitPct == 80 &&
          gState.lastAppliedSmartChg == 5242897u && gState.lastAppliedNightChg == 1);
    check("setChgConfig(5, '4++1+85+1+85') -> limit=85%",
          transactSetChgConfig(b, FLAG_SET_CHG_UP_LIMIT_VALUE, "4++1+85+1+85", 0, ret) &&
          ret == 0 && gState.limitEnabled && gState.limitPct == 85 &&
          gState.lastAppliedSmartChg == packSmartChgEndurance(true, 85) &&
          gState.lastAppliedNightChg == 0);

    // 5. Bypass Charging ON overrides limit -> OFF restores limit 85%!
    check("setChgConfig(25, '1+switch=1') -> smart_chg=65553 (BYPASS)",
          transactSetChgConfig(b, FLAG_SET_CHG_BYPASS, "1+switch=1", 0, ret) &&
          ret == 0 && gState.bypassRequested && gState.lastAppliedSmartChg == 65553u);
    check("setChgConfig(25, '1+switch=0') -> restores 85% limit",
          transactSetChgConfig(b, FLAG_SET_CHG_BYPASS, "1+switch=0", 0, ret) &&
          ret == 0 && !gState.bypassRequested &&
          gState.lastAppliedSmartChg == packSmartChgEndurance(true, 85));
    check("setChgConfig(14, '2++0+85') -> smart_chg=16 (DISABLE)",
          transactSetChgConfig(b, FLAG_SET_CHG_UP_LIMIT_STATE, "2++0+85", 0, ret) &&
          ret == 0 && !gState.limitEnabled && gState.lastAppliedSmartChg == 16u);

    // 6. Lockscreen "Hold to speed up" (mode=4) vs Settings Smart Rapid Charging (mode=3)
    int32_t curMode = -1;
    check("setCustomSelectChgMode(3, true) keeps normal cap (no auto-boost)",
          transactSetCustomMode(b, 3, true, ret) && ret == 0 &&
          transactIntOut(b, TX_getCustomSelectChgMode, curMode) && curMode == 3 &&
          !gState.boostActive);
    check("setCustomSelectChgMode(4, true) activates lockscreen boost",
          transactSetCustomMode(b, 4, true, ret) && ret == 0 &&
          transactIntOut(b, TX_getCustomSelectChgMode, curMode) && curMode == 4 &&
          gState.boostActive);
    check("setCustomSelectChgMode(4, false) stops lockscreen boost",
          transactSetCustomMode(b, 4, false, ret) && ret == 0 &&
          transactIntOut(b, TX_getCustomSelectChgMode, curMode) && curMode == 3 &&
          !gState.boostActive);

    // Thermal curve + hysteresis verification
    int tier = 0;
    tier = updateThermalTier(tier, 350); // 35.0 C
    check("Thermal 35C normal=lvl6(3000mA), boost=lvl0(6000mA/~5.6A)",
          tier == 0 && computeThermalLevel(tier, false, false) == 6 &&
          computeThermalLevel(tier, true, false) == 0);
    tier = updateThermalTier(tier, 405); // 40.5 C
    check("Thermal 40.5C boost tapers to lvl3(4500mA)",
          tier == 1 && computeThermalLevel(tier, true, false) == 3 &&
          computeThermalLevel(tier, false, false) == 6);
    tier = updateThermalTier(tier, 422); // 42.2 C
    check("Thermal 42.2C cools to lvl8(2500mA)",
          tier == 2 && computeThermalLevel(tier, true, false) == 8 &&
          computeThermalLevel(tier, false, false) == 8);
    tier = updateThermalTier(tier, 435); // 43.5 C
    check("Thermal 43.5C cools to lvl11(1800mA)",
          tier == 3 && computeThermalLevel(tier, true, false) == 11);
    tier = updateThermalTier(tier, 442); // 44.2 C
    check("Thermal 44.2C cools to lvl12(1500mA)",
          tier == 4 && computeThermalLevel(tier, true, false) == 12);
    tier = updateThermalTier(tier, 452); // 45.2 C
    check("Thermal 45.2C protects at lvl13(900mA)",
          tier == 5 && computeThermalLevel(tier, true, false) == 13);
    // Hysteresis: 44.0 C stays at tier 5 (> 43.5 C), 43.2 C steps down to tier 4!
    tier = updateThermalTier(tier, 440);
    check("Thermal hysteresis 44.0C holds tier 5", tier == 5);
    tier = updateThermalTier(tier, 432);
    check("Thermal hysteresis 43.2C steps down to tier 4 (1500mA)", tier == 4);
    tier = updateThermalTier(tier, 412);
    check("Thermal hysteresis 41.2C steps down to tier 2 (2500mA)", tier == 2);
    // Cools to 38.0 C -> recovers to tier 0
    tier = updateThermalTier(tier, 380);
    check("Thermal recovery 38.0C returns to tier 0", tier == 0);

    // 7. Live readback sanity
    int32_t soc = -1, temp = 0, vbat = 0, soh = 0;
    transactIntOut(b, TX_getPsyBatteryLevel, soc);
    transactIntOut(b, TX_getPsyBatteryTemp, temp);
    transactIntOut(b, TX_getBatteryVoltageNow, vbat);
    transactIntOut(b, TX_getUIsohValue, soh);
    printf("  Live readback: soc=%d%% temp=%.1fC vbat=%dmV soh=%d%%\n",
           soc, temp / 10.0, vbat, soh);

    AIBinder_decStrong(b);
    printf("[ChargerAidlBridge] Self-test %s!\n", pass ? "PASSED" : "FAILED");
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        return runSelfTest();
    }

    BRIDGE_LOG_BOOT("ChargerAidlBridge starting (tanzanite MT6789 -> ColorOS 17 AIDL V%d ICharger, debug prop=%s)...",
                    kIChargerVersion, cos_bridge_log::kDebugPropName);

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    gChargerClass = AIBinder_Class_define(
            kIChargerDescriptor, onCreateCharger, onDestroyCharger, onTransactCharger);
    AIBinder* chargerBinder = AIBinder_new(gChargerClass, nullptr);
    AIBinder_markVintfStability(chargerBinder);

    binder_status_t status = AServiceManager_addService(chargerBinder, kIChargerInstance);
    if (status != STATUS_OK) {
        ALOGE("AServiceManager_addService(%s) failed: %d", kIChargerInstance, status);
        return 1;
    }

    std::thread supervisor(supervisorThreadMain);
    supervisor.detach();

    BRIDGE_LOG_BOOT("Registered %s (V%d) successfully! Normal cap=3000mA(lvl %d), Boost=5.6A(lvl %d, %llds), Thermal guard=42-45C",
                    kIChargerInstance, kIChargerVersion, kLevelNormalPeak, kLevelBoostMax,
                    static_cast<long long>(kBoostDurationSec));

    ABinderProcess_joinThreadPool();
    ALOGE("ABinderProcess_joinThreadPool() exited unexpectedly!");
    return 1;
}
