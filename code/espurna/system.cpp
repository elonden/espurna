/*

SYSTEM MODULE

Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#include "rtcmem.h"
#include "ws.h"
#include "ntp.h"

#include <cstdint>
#include <cstring>
#include <forward_list>
#include <vector>

extern "C" {
#include "user_interface.h"
extern struct rst_info resetInfo;
}

#include "libs/TypeChecks.h"

// -----------------------------------------------------------------------------

// This method is called by the SDK early on boot to know where to connect the ADC
// Notice that current Core versions automatically de-mangle the function name for historical reasons
// (meaning, it is already used as `_Z14__get_adc_modev` and there's no need for `extern "C"`)
int __get_adc_mode() {
    return (int) (ADC_MODE_VALUE);
}

// Exposed through libphy.a in the current NONOS, may be replaced with a direct call to `os_random()` / `esp_random()`
extern "C" unsigned long adc_rand_noise;

// -----------------------------------------------------------------------------

namespace espurna {
namespace system {
namespace {

namespace settings {
namespace options {

PROGMEM_STRING(None, "none");
PROGMEM_STRING(Once, "once");
PROGMEM_STRING(Repeat, "repeat");

static constexpr espurna::settings::options::Enumeration<heartbeat::Mode> HeartbeatModeOptions[] PROGMEM {
    {heartbeat::Mode::None, None},
    {heartbeat::Mode::Once, Once},
    {heartbeat::Mode::Repeat, Repeat},
};

} // namespace options

namespace keys {

PROGMEM_STRING(Hostname, "hostname");
PROGMEM_STRING(Description, "desc");
PROGMEM_STRING(Password, "adminPass");

} // namespace keys

} // namespace settings
} // namespace
} // namespace system

namespace settings {
namespace internal {

template <>
espurna::heartbeat::Mode convert(const String& value) {
    return convert(system::settings::options::HeartbeatModeOptions, value, heartbeat::Mode::Repeat);
}

String serialize(espurna::heartbeat::Mode mode) {
    return serialize(system::settings::options::HeartbeatModeOptions, mode);
}

template <>
duration::Seconds convert(const String& value) {
    return duration::Seconds(convert<duration::Seconds::rep>(value));
}

String serialize(espurna::duration::Seconds value) {
    return serialize(value.count());
}

template <>
std::chrono::duration<float> convert(const String& value) {
    return std::chrono::duration<float>(convert<float>(value));
}

template <>
duration::Milliseconds convert(const String& value) {
    return duration::Milliseconds(convert<duration::Milliseconds::rep>(value));
}

String serialize(espurna::duration::Milliseconds value) {
    return serialize(value.count());
}

String serialize(espurna::duration::ClockCycles value) {
    return serialize(value.count());
}

} // namespace internal
} // namespace settings

// -----------------------------------------------------------------------------

namespace system {

uint32_t RandomDevice::operator()() const {
    // Repeating SDK source, XORing some ADC-based noise and a HW register exposing the random generator
    // - https://github.com/espressif/ESP8266_RTOS_SDK/blob/d45071563cebe9ca520cbed2537dc840b4d6a1e6/components/esp8266/source/hw_random.c
    // - disassembled source of the `os_random` -> `r_rand` -> `phy_get_rand`
    //   (and avoiding these two additional `call`s)

    // aka WDEV_COUNT_REG, base address
    static constexpr uintptr_t BaseAddress { 0x3ff20c00 };
    // aka WDEV_RAND, the actual register address
    static constexpr uintptr_t Address  { BaseAddress + 0x244 };

    return adc_rand_noise ^ *(reinterpret_cast<volatile uint32_t*>(Address));
}

namespace {

namespace internal {

PROGMEM_STRING(Hostname, HOSTNAME);
PROGMEM_STRING(Password, ADMIN_PASS);

} // namespace internal

StringView chip_id() {
    const static String out = ([]() {
        const uint32_t regs[3] {
            READ_PERI_REG(0x3ff00050),
            READ_PERI_REG(0x3ff00054),
            READ_PERI_REG(0x3ff0005c)};

        uint8_t mac[6] {
            0xff,
            0xff,
            0xff,
            static_cast<uint8_t>((regs[1] >> 8ul) & 0xfful),
            static_cast<uint8_t>(regs[1] & 0xffu),
            static_cast<uint8_t>((regs[0] >> 24ul) & 0xffu)};

        if (mac[2] != 0) {
            mac[0] = (regs[2] >> 16ul) & 0xffu;
            mac[1] = (regs[2] >> 8ul) & 0xffu;
            mac[2] = (regs[2] & 0xffu);
        } else if (0 == ((regs[1] >> 16ul) & 0xff)) {
            mac[0] = 0x18;
            mac[1] = 0xfe;
            mac[2] = 0x34;
        } else if (1 == ((regs[1] >> 16ul) & 0xff)) {
            mac[0] = 0xac;
            mac[1] = 0xd0;
            mac[2] = 0x74;
        }

        return hexEncode(mac);
    })();

    return out;
}

StringView short_chip_id() {
    const auto full = chip_id();
    return StringView(full.begin() + 6, full.end());
}

StringView device() {
    const static String out = ([]() {
        String out;

        const auto hardware = buildHardware();
        out.concat(hardware.manufacturer.c_str(),
            hardware.manufacturer.length());
        out += '_';
        out.concat(hardware.device.c_str(),
            hardware.device.length());

        return out;
    })();

    return out;
}

StringView identifier() {
    const static String out = ([]() {
        String out;

        const auto app = buildApp();
        out.concat(app.name.c_str(), app.name.length());

        out += '-';

        const auto id = short_chip_id();
        out.concat(id.c_str(), id.length());

        return out;
    })();

    return out;
}

String description() {
    return getSetting(settings::keys::Description);
}

String hostname() {
    if (__builtin_strlen(internal::Hostname) > 0) {
        return getSetting(settings::keys::Hostname, internal::Hostname);
    }

    return getSetting(settings::keys::Hostname, identifier());
}

StringView default_password() {
    return internal::Password;
}

String password() {
    return getSetting(settings::keys::Password, default_password());
}

// something rudimentary, just to avoid comparing these strings directly
// ref. `CRYPTO`, `cst_time`, `ct` aka constant time operations.
// might really be useless for us, though, since output can happen
// at almost random times when dealing with LWIP stack and networking requests
#if 0
namespace internal {

using Hash = std::array<uint8_t, 16>;

Hash md5_hash(StringView data) {
    Hash out;

    MD5Builder builder;
    builder.begin();
    builder.add(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    builder.calculate();
    builder.getBytes(out.data());

    return out;
}

} // namespace internal

bool password_equals(StringView other) {
    const auto password = system::password();
    return internal::md5_hash(other)
        == internal::md5_hash(password);
}
#endif

bool password_equals(StringView other) {
    const auto password = system::password();
    return other == password;
}

namespace settings {
namespace query {

static constexpr std::array<espurna::settings::query::Setting, 3> Settings PROGMEM {{
     {keys::Description, system::description},
     {keys::Hostname, system::hostname},
     {keys::Password, system::password},
}};

bool checkExact(StringView key) {
    return espurna::settings::query::Setting::findFrom(Settings, key) != Settings.end();
}

String findValueFrom(StringView key) {
    return espurna::settings::query::Setting::findValueFrom(Settings, key);
}

void setup() {
    settingsRegisterQueryHandler({
        .check = checkExact,
        .get = findValueFrom,
    });
}

} // namespace query
} // namespace settings

} // namespace
} // namespace system

namespace time {

// c/p from the Core 3.1.0, allow an additional calculation, so we don't delay more than necessary
// plus, another helper when there are no external blocking checker

bool tryDelay(CoreClock::time_point start, CoreClock::duration timeout, CoreClock::duration interval) {
    auto elapsed = CoreClock::now() - start;
    if (elapsed < timeout) {
        delay(std::min((timeout - elapsed), interval));
        return false;
    }

    return true;
}

void blockingDelay(CoreClock::duration timeout, CoreClock::duration interval) {
    blockingDelay(timeout, interval, []() {
        return true;
    });
}

void blockingDelay(CoreClock::duration timeout) {
    blockingDelay(timeout, timeout);
}

} // namespace time

namespace timer {

constexpr SystemTimer::Duration SystemTimer::DurationMin;
constexpr SystemTimer::Duration SystemTimer::DurationMax;

SystemTimer::SystemTimer() = default;

void SystemTimer::start(duration::Milliseconds duration, Callback callback, bool repeat) {
    stop();
    if (!duration.count()) {
        return;
    }

    if (!_timer) {
        _timer.reset(new os_timer_t{});
    }
    _armed = _timer.get();

    _callback = std::move(callback);
    _repeat = repeat;

    os_timer_setfn(_timer.get(),
        [](void* arg) {
            reinterpret_cast<SystemTimer*>(arg)->callback();
        },
        this);

    size_t total = 0;
    if (duration > DurationMax) {
        total = 1;
        while (duration > DurationMax) {
            total *= 2;
            duration /= 2;
        }
        _tick.reset(new Tick{
            .total = total,
            .count = 0,
        });
        repeat = true;
    }

    os_timer_arm(_armed, duration.count(), repeat);
}

void SystemTimer::stop() {
    if (_armed) {
        os_timer_disarm(_armed);
    }
    reset();
}

void SystemTimer::reset() {
    _armed = nullptr;
    _callback = Callback();
    _tick = nullptr;
}

void SystemTimer::callback() {
    if (_tick) {
        ++_tick->count;
        if (_tick->count < _tick->total) {
            return;
        }
    }

    _callback();

    if (_repeat) {
        if (_tick) {
            _tick->count = 0;
        }
        return;
    }

    stop();
}

void SystemTimer::schedule_once(Duration duration, Callback callback) {
    once(duration, [callback]() {
        espurnaRegisterOnce(callback);
    });
}

} // namespace timer

namespace {

namespace memory {

// returns 'total stack size' minus 'un-painted area'
// needs re-painting step, as this never decreases
size_t freeStack() {
    return ESP.getFreeContStack();
}

// esp8266 normally only has a one single heap area, located in DRAM just 'before' the SYS stack
// since Core 3.x.x, internal C-level allocator was extended to support multiple contexts
// - external SPI RAM chip (but, this may not work with sizes above 65KiB on older Cores, check the actual version)
// - part of the IRAM, which will be specifically excluded from the CACHE by using a preprocessed linker file
//
// API expects us to use the same C API as usual - malloc, realloc, calloc, etc.
// Only now we are able to switch 'contexts' and receive different address range, currenty via `umm_{push,pop}_heap(ID)`
// (e.g. UMM_HEAP_DRAM, UMM_HEAP_IRAM, ... which techically is an implementation detail, and ESP::... methods should be used)
//
// Meaning, what happens below is heavily dependant on the when and why these functions are called

size_t freeHeap() {
    return system_get_free_heap_size();
}

decltype(freeHeap()) initialFreeHeap() {
    static const auto value = ([]() {
        return system_get_free_heap_size();
    })();

    return value;
}

// see https://github.com/esp8266/Arduino/pull/8440
template <typename T>
using HasHeapStatsFixBase = decltype(std::declval<T>().getHeapStats(
    std::declval<uint32_t*>(), std::declval<uint32_t*>(), std::declval<uint8_t*>()));

template <typename T>
using HasHeapStatsFix = is_detected<HasHeapStatsFixBase, T>;

template <typename T>
HeapStats heapStats(T& instance, std::true_type) {
    HeapStats out;
    instance.getHeapStats(&out.available, &out.usable, &out.fragmentation);
    return out;
}

template <typename T>
HeapStats heapStats(T& instance, std::false_type) {
    HeapStats out;
    uint16_t usable{0};
    instance.getHeapStats(&out.available, &usable, &out.fragmentation);
    out.usable = usable;
    return out;
}

HeapStats heapStats() {
    return heapStats(ESP, HasHeapStatsFix<EspClass>{});
}

} // namespace memory

namespace boot {

String serialize(CustomResetReason reason) {
    const char* ptr { PSTR("None") };

    switch (reason) {
    case CustomResetReason::None:
        break;
    case CustomResetReason::Button:
        ptr = PSTR("Hardware button");
        break;
    case CustomResetReason::Factory:
        ptr = PSTR("Factory reset");
        break;
    case CustomResetReason::Hardware:
        ptr = PSTR("Reboot from a Hardware request");
        break;
    case CustomResetReason::Mqtt:
        ptr = PSTR("Reboot from MQTT");
        break;
    case CustomResetReason::Ota:
        ptr = PSTR("Reboot after a successful OTA update");
        break;
    case CustomResetReason::Rpc:
        ptr = PSTR("Reboot from a RPC action");
        break;
    case CustomResetReason::Rule:
        ptr = PSTR("Reboot from an automation rule");
        break;
    case CustomResetReason::Scheduler:
        ptr = PSTR("Reboot from a scheduler action");
        break;
    case CustomResetReason::Terminal:
        ptr = PSTR("Reboot from a terminal command");
        break;
    case CustomResetReason::Web:
        ptr = PSTR("Reboot from web interface");
        break;
    case CustomResetReason::Stability:
        ptr = PSTR("Reboot after changing stability counter");
        break;
    }

    return ptr;
}

// The ESPLive has an ADC MUX which needs to be configured.
// Default CT input (pin B, solder jumper B)
void hardware() {
#if defined(MANCAVEMADE_ESPLIVE)
    pinMode(16, OUTPUT);
    digitalWrite(16, HIGH);
#endif
}

// If the counter reaches SYSTEM_CHECK_MAX then the system is flagged as unstable
// When it that mode, system will only have minimal set of services available
struct Data {
    Data() = delete;
    explicit Data(volatile uint32_t* ptr) :
        _ptr(ptr)
    {}

    explicit operator bool() const {
        return rtcmemStatus();
    }

    uint8_t counter() const {
        return read().counter;
    }

    void counter(uint8_t input) {
        auto value = read();
        value.counter = input;
        write(value);
    }

    CustomResetReason reason() const {
        return static_cast<CustomResetReason>(read().reason);
    }

    void reason(CustomResetReason input) {
        auto value = read();
        value.reason = static_cast<uint8_t>(input);
        write(value);
    }

    uint32_t value() const {
        return *_ptr;
    }

private:
    struct alignas(uint32_t) Raw {
        uint8_t counter;
        uint8_t reason;
        uint8_t _stub1;
        uint8_t _stub2;
    };

    static_assert(sizeof(Raw) == sizeof(uint32_t), "");
    static_assert(alignof(Raw) == alignof(uint32_t), "");

    void write(Raw raw) {
        uint32_t out{};
        std::memcpy(&out, &raw, sizeof(out));
        *_ptr = out;
    }

    Raw read() const {
        uint32_t value = *_ptr;

        Raw out{};
        std::memcpy(&out, &value, sizeof(out));

        return out;
    }

    volatile uint32_t* _ptr;
};

namespace internal {

Data persistent_data { &Rtcmem->sys };

timer::SystemTimer timer;
bool flag { true };

} // namespace internal

// system_get_rst_info() result is cached by the Core init for internal use
uint32_t system_reason() {
    return resetInfo.reason;
}

// prunes custom reason after accessing it once
CustomResetReason customReason() {
    static const CustomResetReason reason = ([]() {
        const auto out = static_cast<bool>(internal::persistent_data)
            ? internal::persistent_data.reason()
            : CustomResetReason::None;
        internal::persistent_data.reason(CustomResetReason::None);
        return out;
    })();

    return reason;
}

void customReason(CustomResetReason reason) {
    internal::persistent_data.reason(reason);
}

#if SYSTEM_CHECK_ENABLED
namespace stability {
namespace build {

static constexpr uint8_t ChecksMin { 1 };
static constexpr uint8_t ChecksMax { SYSTEM_CHECK_MAX };

static constexpr uint8_t ChecksIncrement { 1 };

static_assert(ChecksMax > 1, "");
static_assert(ChecksMin < ChecksMax, "");

constexpr espurna::duration::Seconds CheckTime { SYSTEM_CHECK_TIME };
static_assert(CheckTime > espurna::duration::Seconds::min(), "");

} // namespace build

void force_stable() {
    internal::persistent_data.counter(build::ChecksMin);
    internal::flag = true;
}

void force_unstable() {
    internal::persistent_data.counter(build::ChecksMax);
    internal::flag = false;
}

uint8_t counter() {
    return static_cast<bool>(internal::persistent_data)
        ? internal::persistent_data.counter()
        : build::ChecksMin;
}

void reset() {
    DEBUG_MSG_P(PSTR("[MAIN] Resetting stability counter\n"));
    internal::persistent_data.counter(build::ChecksMin);
}

void init() {
    const auto count = counter();

    switch (system_reason()) {
    // initial boot and rst are probably just fine
    case REASON_DEFAULT_RST:
    case REASON_EXT_SYS_RST:
        force_stable();
        return;
    // no need to run the timer when counter gets changed manually
    case REASON_SOFT_RESTART:
        if (customReason() == CustomResetReason::Stability) {
            internal::flag = (count < build::ChecksMax);
            return;
        }
        break;
    }

    // bump counter value and persist. if we re-enter with maximum
    // once more, system is flagged as unstable.
    // so, we simply wait for the timer to reset back to minimum
    // and start the cycle again
    const auto next = std::min(build::ChecksMax,
        static_cast<uint8_t>(count + build::ChecksIncrement));
    internal::persistent_data.counter(next);
    internal::flag = (count < build::ChecksMax);

    internal::timer.once(build::CheckTime, reset);
}

bool check() {
    return internal::flag;
}

} // namespace stability
#endif

void pre() {
    // Some magic to allow seamless Tasmota OTA upgrades
    // - inject dummy data sequence that is expected to hold current version info
    // - purge settings, since we don't want accidentaly reading something as a kv
    // - sometimes we cannot boot b/c of certain SDK params, purge last 16KiB
    {
        // ref. `SetOption78 1` in Tasmota
        // - https://tasmota.github.io/docs/Commands/#setoptions (> SetOption78   Version check on Tasmota upgrade)
        // - https://github.com/esphome/esphome/blob/0e59243b83913fc724d0229514a84b6ea14717cc/esphome/core/esphal.cpp#L275-L287 (the original idea from esphome)
        // - https://github.com/arendst/Tasmota/blob/217addc2bb2cf46e7633c93e87954b245cb96556/tasmota/settings.ino#L218-L262 (specific checks, which succeed when finding 0xffffffff as version)
        // - https://github.com/arendst/Tasmota/blob/0dfa38df89c8f2a1e582d53d79243881645be0b8/tasmota/i18n.h#L780-L782 (constants)
        volatile uint32_t magic[] __attribute__((unused)) {
            0x5aa55aa5,
            0xffffffff,
            0xa55aa55a,
        };

        // ref. https://github.com/arendst/Tasmota/blob/217addc2bb2cf46e7633c93e87954b245cb96556/tasmota/settings.ino#L24
        // We will certainly find these when rebooting from Tasmota. Purge SDK as well, since we may experience WDT after starting up the softAP
        auto* rtcmem = reinterpret_cast<volatile uint32_t*>(RTCMEM_ADDR);
        if ((0xA55A == rtcmem[64]) && (0xA55A == rtcmem[68])) {
            DEBUG_MSG_P(PSTR("[MAIN] TASMOTA OTA, resetting...\n"));
            rtcmem[64] = rtcmem[68] = 0;
            customResetReason(CustomResetReason::Factory);
            resetSettings();
            eraseSDKConfig();
            __builtin_trap();
            // can't return!
        }

        // TODO: also check for things throughout the flash sector, somehow?
    }

    // Workaround for SDK changes between 1.5.3 and 2.2.x or possible
    // flash corruption happening to the 'default' WiFi config
#if SYSTEM_CHECK_ENABLED
    if (!stability::check()) {
        const uint32_t Address { ESP.getFlashChipSize() - (FLASH_SECTOR_SIZE * 3) };

        static constexpr size_t PageSize { 256 };
#ifdef FLASH_PAGE_SIZE
        static_assert(FLASH_PAGE_SIZE == PageSize, "");
#endif
        static constexpr auto Alignment = alignof(uint32_t);
        alignas(Alignment) std::array<uint8_t, PageSize> page;

        if (ESP.flashRead(Address, reinterpret_cast<uint32_t*>(page.data()), page.size())) {
            constexpr uint32_t ConfigOffset { 0xb0 };

            // In case flash was already erased at some point, but we are still here
            alignas(Alignment) const std::array<uint8_t, 8> Empty { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            if (std::memcpy(&page[ConfigOffset], &Empty[0], Empty.size()) != 0) {
                return;
            }

            // 0x00B0:  0A 00 00 00 45 53 50 2D XX XX XX XX XX XX 00 00     ESP-XXXXXX
            alignas(Alignment) const std::array<uint8_t, 8> Reference { 0xa0, 0x00, 0x00, 0x00, 0x45, 0x53, 0x50, 0x2d };
            if (std::memcmp(&page[ConfigOffset], &Reference[0], Reference.size()) != 0) {
                DEBUG_MSG_P(PSTR("[MAIN] Invalid SDK config at 0x%08X, resetting...\n"), Address + ConfigOffset);
                customResetReason(CustomResetReason::Factory);
                systemForceStable();
                forceEraseSDKConfig();
                // can't return!
            }
        }
    }
#endif
}

} // namespace boot

// -----------------------------------------------------------------------------

// Calculated load average of the loop() as a percentage (notice that this may not be accurate)
namespace load_average {
namespace build {

static constexpr size_t ValueMax { 100 };

static constexpr espurna::duration::Seconds Interval { LOADAVG_INTERVAL };
static_assert(Interval <= espurna::duration::Seconds(90), "");

} // namespace build

using TimeSource = espurna::time::SystemClock;
using Type = unsigned long;

struct Counter {
    TimeSource::time_point last;
    Type count;
    Type value;
    Type max;
};

namespace internal {

Type load_average { 0 };

} // namespace internal

Type value() {
    return internal::load_average;
}

void loop() {
    static Counter counter {
        .last = (TimeSource::now() - build::Interval),
        .count = 0,
        .value = 0,
        .max = 0
    };

    ++counter.count;

    const auto timestamp = TimeSource::now();
    if (timestamp - counter.last < build::Interval) {
        return;
    }

    counter.last = timestamp;
    counter.value = counter.count;
    counter.count = 0;
    counter.max = std::max(counter.max, counter.value);

    internal::load_average = counter.max
        ? (build::ValueMax - (build::ValueMax * counter.value / counter.max))
        : 0;
}

} // namespace load_average
} // namespace

// -----------------------------------------------------------------------------

namespace heartbeat {
namespace {

namespace build {

constexpr Mode mode() {
    return HEARTBEAT_MODE;
}

constexpr espurna::duration::Seconds interval() {
    return espurna::duration::Seconds { HEARTBEAT_INTERVAL };
}

constexpr Mask value() {
    return (Report::Status * (HEARTBEAT_REPORT_STATUS))
        | (Report::Ssid * (HEARTBEAT_REPORT_SSID))
        | (Report::Ip * (HEARTBEAT_REPORT_IP))
        | (Report::Mac * (HEARTBEAT_REPORT_MAC))
        | (Report::Rssi * (HEARTBEAT_REPORT_RSSI))
        | (Report::Uptime * (HEARTBEAT_REPORT_UPTIME))
        | (Report::Datetime * (HEARTBEAT_REPORT_DATETIME))
        | (Report::Freeheap * (HEARTBEAT_REPORT_FREEHEAP))
        | (Report::Vcc * (HEARTBEAT_REPORT_VCC))
        | (Report::Relay * (HEARTBEAT_REPORT_RELAY))
        | (Report::Light * (HEARTBEAT_REPORT_LIGHT))
        | (Report::Hostname * (HEARTBEAT_REPORT_HOSTNAME))
        | (Report::Description * (HEARTBEAT_REPORT_DESCRIPTION))
        | (Report::App * (HEARTBEAT_REPORT_APP))
        | (Report::Version * (HEARTBEAT_REPORT_VERSION))
        | (Report::Board * (HEARTBEAT_REPORT_BOARD))
        | (Report::Loadavg * (HEARTBEAT_REPORT_LOADAVG))
        | (Report::Interval * (HEARTBEAT_REPORT_INTERVAL))
        | (Report::Range * (HEARTBEAT_REPORT_RANGE))
        | (Report::RemoteTemp * (HEARTBEAT_REPORT_REMOTE_TEMP))
        | (Report::Bssid * (HEARTBEAT_REPORT_BSSID));
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Mode, "hbMode");
PROGMEM_STRING(Interval, "hbInterval");
PROGMEM_STRING(Report, "hbReport");

} // namespace keys

Mode mode() {
    return getSetting(keys::Mode, build::mode());
}

espurna::duration::Seconds interval() {
    return getSetting(keys::Interval, build::interval());
}

Mask value() {
    // because we start shifting from 1, we could use the
    // first bit as a flag to enable all of the messages
    static constexpr Mask MaskAll { 1 };

    auto value = getSetting(keys::Report, build::value());
    if (value == MaskAll) {
        value = std::numeric_limits<Mask>::max();
    }

    return value;
}

} // namespace settings

using TimeSource = espurna::time::CoreClock;

struct CallbackRunner {
    Callback callback;
    Mode mode;
    TimeSource::duration interval;
    TimeSource::time_point last;
};

namespace internal {

timer::SystemTimer timer;
std::vector<CallbackRunner> runners;
bool scheduled { false };

} // namespace internal

void schedule() {
    internal::scheduled = true;
}

bool scheduled() {
    if (internal::scheduled) {
        internal::scheduled = false;
        return true;
    }

    return false;
}

void run() {
    static constexpr duration::Milliseconds BeatMin { duration::Seconds(1) };
    static constexpr duration::Milliseconds BeatMax { BeatMin * 10 };

    auto next = duration::Milliseconds(settings::interval());

    if (internal::runners.size()) {
        auto mask = settings::value();

        auto it = internal::runners.begin();
        auto end = internal::runners.end();

        auto ts = TimeSource::now();
        while (it != end) {
            auto diff = ts - (*it).last;
            if (diff > (*it).interval) {
                auto result = (*it).callback(mask);
                if (result && ((*it).mode == Mode::Once)) {
                    it = internal::runners.erase(it);
                    end = internal::runners.end();
                    continue;
                }

                if (result) {
                    (*it).last = ts;
                } else if (diff < ((*it).interval + BeatMax)) {
                    next = BeatMin;
                }

                next = std::min(next, (*it).interval);
            } else {
                next = std::min(next, (*it).interval - diff);
            }
            ++it;
        }
    }

    if (next < BeatMin) {
        next = BeatMin;
    }

    internal::timer.once(next, schedule);
}

void stop(Callback callback) {
    auto found = std::remove_if(
        internal::runners.begin(),
        internal::runners.end(),
        [&](const CallbackRunner& runner) {
            return callback == runner.callback;
        });
    internal::runners.erase(found, internal::runners.end());
}

void push(Callback callback, Mode mode, duration::Seconds interval) {
    if (mode == Mode::None) {
        return;
    }

    auto msec = duration::Milliseconds(interval);
    if ((mode != Mode::Once) && !msec.count()) {
        return;
    }

    auto offset = TimeSource::now() - TimeSource::duration(1);
    internal::runners.push_back({
        callback, mode,
        msec,
        offset - msec
    });

    internal::timer.stop();
    schedule();
}

void pushOnce(Callback callback) {
    push(callback, Mode::Once, espurna::duration::Seconds::min());
}

duration::Seconds interval() {
    TimeSource::duration result { settings::interval() };

    for (auto& runner : internal::runners) {
        if (runner.mode != Mode::Once) {
            result = std::min(result, runner.interval);
        }
    }

    return std::chrono::duration_cast<duration::Seconds>(result);
}

void reschedule() {
    static constexpr TimeSource::duration Offset { 1 };

    const auto ts = TimeSource::now();
    for (auto& runner : internal::runners) {
        runner.last = ts - runner.interval - Offset;
    }

    schedule();
}

void loop() {
    if (scheduled()) {
        run();
    }
}

void init() {
#if DEBUG_SUPPORT
    pushOnce([](Mask) {
        const auto mode = settings::mode();
        if (mode != Mode::None) {
            DEBUG_MSG_P(PSTR("[MAIN] Heartbeat \"%s\", every %u (seconds)\n"),
                espurna::settings::internal::serialize(mode).c_str(),
                settings::interval().count());
        } else {
            DEBUG_MSG_P(PSTR("[MAIN] Heartbeat disabled\n"));
        }
        return true;
    });
#if SYSTEM_CHECK_ENABLED
    pushOnce([](Mask) {
        if (!espurna::boot::stability::check()) {
            DEBUG_MSG_P(PSTR("[MAIN] System UNSTABLE\n"));
        } else if (espurna::boot::internal::timer) {
            DEBUG_MSG_P(PSTR("[MAIN] Pending stability counter reset...\n"));
        }
        return true;
    });
#endif
#endif
    schedule();
}

} // namespace

// system defaults, r/n this is used when providing module-specific settings

espurna::duration::Milliseconds currentIntervalMs() {
    return settings::interval();
}

espurna::duration::Seconds currentInterval() {
    return settings::interval();
}

Mask currentValue() {
    return settings::value();
}

Mode currentMode() {
    return settings::mode();
}

} // namespace heartbeat

namespace {

#if WEB_SUPPORT
namespace web {

void onConnected(JsonObject& root) {
  root[FPSTR(heartbeat::settings::keys::Report)] = heartbeat::settings::value();
  root[FPSTR(heartbeat::settings::keys::Interval)] =
      heartbeat::settings::interval().count();
  root[FPSTR(heartbeat::settings::keys::Mode)] =
      espurna::settings::internal::serialize(heartbeat::settings::mode());
}

bool onKeyCheck(StringView key, const JsonVariant&) {
    return espurna::settings::query::samePrefix(key, STRING_VIEW("sys"))
        || espurna::settings::query::samePrefix(key, STRING_VIEW("hb"));
}

void init() {
    wsRegister()
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
}

} // namespace web
#endif

// Allow to schedule a reset at the next loop
// Store reset reason both here and in for the next boot
namespace internal {

timer::SystemTimer reset_timer;
auto reset_reason = CustomResetReason::None;

void reset(CustomResetReason reason) {
    ::espurna::boot::customReason(reason);
    reset_reason = reason;
}

} // namespace internal

// raw reboot call, effectively:
// ```
// system_restart();
// esp_suspend();
// ```
// triggered in SYS, might not always result in a clean reboot b/c of expected suspend
// triggered in CONT *should* end up never returning back and loop might now be needed
[[noreturn]] void reset() {
    ESP.restart();
    __builtin_trap();
}

// 'simple' reboot call with software controlled time
// always needs a reason, so it can be displayed in logs and / or trigger some actions on boot
void pending_reset_loop() {
    if (internal::reset_reason != CustomResetReason::None) {
        reset();
    }
}

static constexpr espurna::duration::Milliseconds ShortDelayForReset { 500 };

void deferredReset(duration::Milliseconds delay, CustomResetReason reason) {
    DEBUG_MSG_P(PSTR("[MAIN] Requested reset: %s\n"),
        espurna::boot::serialize(reason).c_str());
    internal::reset_timer.once(
        delay,
        [reason]() {
            internal::reset(reason);
        });
}

// SDK reserves last 16KiB on the flash for it's own means
// Notice that it *may* also be required to soft-crash the board,
// so it does not end up restoring the configuration cached in RAM
// ref. https://github.com/esp8266/Arduino/issues/1494
bool eraseSDKConfig() {
    return ESP.eraseConfig();
}

void forceEraseSDKConfig() {
    eraseSDKConfig();
    __builtin_trap();
}

// Accumulates only when called, make sure to do so periodically
// Even in 32bit range, seconds would take a lot of time to overflow
duration::Seconds uptime() {
    return std::chrono::duration_cast<duration::Seconds>(
        time::SystemClock::now().time_since_epoch());
}

void loop() {
    pending_reset_loop();
    load_average::loop();
    heartbeat::loop();
}

void setup() {
    boot::pre();

    boot::hardware();
    boot::customReason();

#if SYSTEM_CHECK_ENABLED
    boot::stability::init();
#endif

#if WEB_SUPPORT
    web::init();
#endif

    system::settings::query::setup();

    espurnaRegisterLoop(loop);
    heartbeat::init();
}

} // namespace
} // namespace espurna

// -----------------------------------------------------------------------------

unsigned long systemFreeStack() {
    return espurna::memory::freeStack();
}

HeapStats systemHeapStats() {
    return espurna::memory::heapStats();
}

size_t systemFreeHeap() {
    return espurna::memory::freeHeap();
}

size_t systemInitialFreeHeap() {
    return espurna::memory::initialFreeHeap();
}

unsigned long systemLoadAverage() {
    return espurna::load_average::value();
}

void reset() {
    espurna::reset();
}

bool eraseSDKConfig() {
    return espurna::eraseSDKConfig();
}

void forceEraseSDKConfig() {
    espurna::forceEraseSDKConfig();
}

void factoryReset() {
    resetSettings();
    espurna::deferredReset(
        espurna::ShortDelayForReset,
        CustomResetReason::Factory);
}

void deferredReset(espurna::duration::Milliseconds delay, CustomResetReason reason) {
    espurna::deferredReset(delay, reason);
}

void prepareReset(CustomResetReason reason) {
    espurna::deferredReset(espurna::ShortDelayForReset, reason);
}

bool pendingDeferredReset() {
    return espurna::internal::reset_reason != CustomResetReason::None;
}

uint32_t systemResetReason() {
    return espurna::boot::system_reason();
}

CustomResetReason customResetReason() {
    return espurna::boot::customReason();
}

void customResetReason(CustomResetReason reason) {
    espurna::boot::customReason(reason);
}

String customResetReasonToPayload(CustomResetReason reason) {
    return espurna::boot::serialize(reason);
}

#if SYSTEM_CHECK_ENABLED
uint8_t systemStabilityCounter() {
    return espurna::boot::internal::persistent_data.counter();
}

void systemStabilityCounter(uint8_t count) {
    espurna::boot::internal::persistent_data.counter(count);
}

void systemForceUnstable() {
    espurna::boot::stability::force_unstable();
}

void systemForceStable() {
    espurna::boot::stability::force_stable();
}

bool systemCheck() {
    return espurna::boot::stability::check();
}
#endif

void systemStopHeartbeat(espurna::heartbeat::Callback callback) {
    espurna::heartbeat::stop(callback);
}

void systemHeartbeat(espurna::heartbeat::Callback callback, espurna::heartbeat::Mode mode, espurna::duration::Seconds interval) {
    espurna::heartbeat::push(callback, mode, interval);
}

void systemHeartbeat(espurna::heartbeat::Callback callback, espurna::heartbeat::Mode mode) {
    espurna::heartbeat::push(callback, mode,
        espurna::heartbeat::settings::interval());
}

void systemHeartbeat(espurna::heartbeat::Callback callback) {
    espurna::heartbeat::push(callback,
        espurna::heartbeat::settings::mode(),
        espurna::heartbeat::settings::interval());
}

espurna::duration::Seconds systemHeartbeatInterval() {
    return espurna::heartbeat::interval();
}

void systemScheduleHeartbeat() {
    espurna::heartbeat::reschedule();
}

espurna::duration::Seconds systemUptime() {
    return espurna::uptime();
}

espurna::StringView systemDevice() {
    return espurna::system::device();
}

espurna::StringView systemIdentifier() {
    return espurna::system::identifier();
}

espurna::StringView systemChipId() {
    return espurna::system::chip_id();
}

espurna::StringView systemShortChipId() {
    return espurna::system::short_chip_id();
}

espurna::StringView systemDefaultPassword() {
    return espurna::system::default_password();
}

String systemPassword() {
    return espurna::system::password();
}

bool systemPasswordEquals(espurna::StringView other) {
    return espurna::system::password_equals(other);
}

String systemHostname() {
    return espurna::system::hostname();
}

String systemDescription() {
    return espurna::system::description();
}

void systemSetup() {
    espurna::setup();
}
