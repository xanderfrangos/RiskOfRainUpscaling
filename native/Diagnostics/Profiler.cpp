#include "Diagnostics/Profiler.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// See Profiler.h. GPU frames use one disjoint query and up to
// MaxEntries timestamps each; results are read back without stalling, several
// frames later, and dropped when every frame slot is still in flight.
namespace {
constexpr size_t FrameSlots = 8, MaxEntries = 512;
constexpr double ReportSeconds = 2.0;
enum Kind { Begin, End, Instant };

std::atomic<bool> enabled{false}, bound{false}, bridgePresents{false};
thread_local bool issuing = false;
struct Issuing { bool previous = issuing; Issuing() { issuing = true; } ~Issuing() { issuing = previous; } };

std::mutex labelMutex;
std::vector<std::string> names;
std::map<std::string, int> ids;
std::map<std::pair<int, int>, int> segments;
std::string Name(int label) { std::lock_guard<std::mutex> guard(labelMutex); return label >= 0 && label < static_cast<int>(names.size()) ? names[label] : "?"; }

struct FrameSet {
    ComPtr<ID3D11Query> disjoint;
    std::vector<ComPtr<ID3D11Query>> queries;
    std::vector<int> labels, kinds;
    size_t used = 0;
    bool open = false, pending = false;
    unsigned generation = 0;
};
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<FrameSet, FrameSlots> sets;
    std::deque<size_t> pending;
    size_t current = SIZE_MAX, next = 0;
} gpu;
std::mutex gpuMutex;

struct Stat {
    // GPU timestamps: per-frame sums, frames the step ran, calls, start offset in the frame.
    double gpuSum = 0, gpuMax = 0, offsetSum = 0; unsigned gpuFrames = 0, gpuCalls = 0; int depth = 0; bool segment = false;
    double cpuSum = 0, cpuMax = 0; unsigned cpuCalls = 0;      // CPU milliseconds per call
    double extSum = 0, extMax = 0; unsigned extCalls = 0;      // D3D12 GPU milliseconds per sample
};
struct Window {
    std::map<int, Stat> stats;
    unsigned frames = 0, presents = 0, dropped = 0, discarded = 0, overflow = 0;
    double spanSum = 0, spanMax = 0, modSum = 0, modMax = 0;
    LARGE_INTEGER started{};
} window;
std::mutex statsMutex;
std::string context = "unspecified";
std::atomic<unsigned> generation{1};
LARGE_INTEGER frequency{};

std::mutex fileMutex;
HANDLE file = INVALID_HANDLE_VALUE;
void Write(const std::string& text)
{
    std::lock_guard<std::mutex> guard(fileMutex);
    if (file == INVALID_HANDLE_VALUE) {
        if (g_logFolder.empty()) return;
        file = CreateFileW((g_logFolder + L"\\profile.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written{}; WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
void Append(std::string& text, const char* format, ...)
{
    char line[1024]; va_list args; va_start(args, format);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    text += line;
}
double Since(const LARGE_INTEGER& start)
{
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// Caller holds statsMutex.
void Report(const char* reason)
{
    auto& w = window;
    if (!w.frames && !w.presents) { QueryPerformanceCounter(&w.started); return; }
    const double seconds = (std::max)(Since(w.started), 1e-6);
    SYSTEMTIME now{}; GetSystemTime(&now);
    std::string text;
    Append(text, "=== PROFILE %04u-%02u-%02uT%02u:%02u:%02uZ window=%.2fs (%s)\r\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, seconds, reason);
    Append(text, "config: %s\r\n", context.c_str());
    Append(text, "D3D11 presents=%u (%.1f/s) frames timed=%u dropped(in flight)=%u discarded(disjoint/config change)=%u overflow=%u\r\n",
        w.presents, w.presents / seconds, w.frames, w.dropped, w.discarded, w.overflow);
    if (w.frames) {
        Append(text, "GPU span, first timed event -> Present: avg %.3f ms, max %.3f ms (wall time on the GPU clock, includes idle gaps)\r\n", w.spanSum / w.frames, w.spanMax);
        Append(text, "Mod-owned GPU work (sum of top-level mod steps): avg %.3f ms/frame, max %.3f ms\r\n", w.modSum / w.frames, w.modMax);
    }
    std::vector<std::pair<int, const Stat*>> gpuRows, cpuRows, extRows;
    for (auto& item : w.stats) {
        if (item.second.gpuFrames) gpuRows.push_back({item.first, &item.second});
        else if (item.second.cpuCalls) cpuRows.push_back({item.first, &item.second});
        if (item.second.extCalls) extRows.push_back({item.first, &item.second});
    }
    // Timeline order: average start offset within the frame.
    std::sort(gpuRows.begin(), gpuRows.end(), [](auto& a, auto& b) {
        double x = a.second->offsetSum / a.second->gpuFrames, y = b.second->offsetSum / b.second->gpuFrames;
        return x != y ? x < y : a.second->depth < b.second->depth;
    });
    if (!gpuRows.empty()) {
        Append(text, "GPU (D3D11 timestamps). [a -> b] = Unity camera segment between two camera events; indented = mod steps inside it.\r\n");
        Append(text, "  gpu avg   gpu max   seen  calls/f  cpu avg   step\r\n");
        for (auto& row : gpuRows) {
            auto& s = *row.second;
            char cpu[32] = "      -";
            if (s.cpuCalls) sprintf_s(cpu, "%7.3f", s.cpuSum / s.cpuCalls);
            Append(text, "  %7.3f   %7.3f   %3.0f%%  %6.2f  %s   %*s%s\r\n", s.gpuSum / s.gpuFrames, s.gpuMax, 100.0 * s.gpuFrames / (std::max)(w.frames, 1u),
                static_cast<double>(s.gpuCalls) / s.gpuFrames, cpu, s.segment ? 0 : 2 * (std::max)(s.depth, 1), "", Name(row.first).c_str());
        }
    }
    if (!extRows.empty()) {
        Append(text, "D3D12 presentation queue (Frame Generation bridge timestamps, per D3D11 Present)\r\n");
        Append(text, "  gpu avg   gpu max   samples  step\r\n");
        for (auto& row : extRows)
            Append(text, "  %7.3f   %7.3f   %7u  %s\r\n", row.second->extSum / row.second->extCalls, row.second->extMax, row.second->extCalls, Name(row.first).c_str());
    }
    if (!cpuRows.empty()) {
        Append(text, "CPU (render/present thread)\r\n");
        Append(text, "  cpu avg   cpu max   calls/s  step\r\n");
        for (auto& row : cpuRows)
            Append(text, "  %7.3f   %7.3f   %7.1f  %s\r\n", row.second->cpuSum / row.second->cpuCalls, row.second->cpuMax, row.second->cpuCalls / seconds, Name(row.first).c_str());
    }
    text += "\r\n";
    Write(text);
    w = Window{}; QueryPerformanceCounter(&w.started);
}

int SegmentLabel(int from, int to)
{
    {
        std::lock_guard<std::mutex> guard(labelMutex);
        auto found = segments.find({from, to});
        if (found != segments.end()) return found->second;
    }
    int id = ProfilerLabel(("[" + Name(from) + " -> " + Name(to) + "]").c_str());
    std::lock_guard<std::mutex> guard(labelMutex);
    segments[{from, to}] = id;
    return id;
}

void ProcessFrame(const FrameSet& set, const std::vector<UINT64>& times, UINT64 ticks)
{
    struct Open { int label; UINT64 time; };
    struct Sum { double ms = 0, offset = 0; unsigned calls = 0; int depth = 0; bool segment = false; };
    auto ms = [&](UINT64 a, UINT64 b) { return b > a ? 1000.0 * static_cast<double>(b - a) / static_cast<double>(ticks) : 0.0; };
    const UINT64 start = times.front();
    std::vector<Open> stack;
    std::map<int, Sum> frame;
    double mod = 0;
    bool segmented = false;
    for (size_t i = 0; i < set.used; ++i) if (set.kinds[i] == Instant) { segmented = true; break; }
    int lastMark = -1; UINT64 lastTime = 0;
    auto add = [&](int label, UINT64 from, UINT64 to, int depth, bool segment) {
        auto& sum = frame[label];
        if (!sum.calls) { sum.offset = ms(start, from); sum.depth = depth; sum.segment = segment; }
        sum.ms += ms(from, to); ++sum.calls;
    };
    for (size_t i = 0; i < set.used; ++i) {
        const int label = set.labels[i];
        switch (set.kinds[i]) {
        case Begin: stack.push_back({label, times[i]}); break;
        case End: {
            auto found = std::find_if(stack.rbegin(), stack.rend(), [&](const Open& o) { return o.label == label; });
            if (found == stack.rend()) break;
            const size_t index = static_cast<size_t>(std::distance(found, stack.rend())) - 1;
            add(label, found->time, times[i], static_cast<int>(index) + (segmented ? 1 : 0), false);
            if (index == 0) mod += ms(found->time, times[i]);
            stack.resize(index);
            break;
        }
        case Instant:
            if (lastMark >= 0) add(SegmentLabel(lastMark, label), lastTime, times[i], 0, true);
            lastMark = label; lastTime = times[i];
            break;
        }
    }
    const double span = ms(start, times[set.used - 1]);
    std::lock_guard<std::mutex> guard(statsMutex);
    if (set.generation != generation.load()) { ++window.discarded; return; }
    ++window.frames;
    window.spanSum += span; window.spanMax = (std::max)(window.spanMax, span);
    window.modSum += mod; window.modMax = (std::max)(window.modMax, mod);
    for (auto& item : frame) {
        auto& s = window.stats[item.first];
        s.gpuSum += item.second.ms; s.gpuMax = (std::max)(s.gpuMax, item.second.ms);
        s.offsetSum += item.second.offset; ++s.gpuFrames; s.gpuCalls += item.second.calls;
        s.depth = item.second.depth; s.segment = item.second.segment;
    }
}

// Caller holds gpuMutex.
void Release()
{
    gpu.pending.clear(); gpu.current = SIZE_MAX; gpu.next = 0;
    for (auto& set : gpu.sets) set = FrameSet{};
    gpu.context.Reset(); gpu.device.Reset(); bound = false;
}
FrameSet* OpenFrame()
{
    if (gpu.current != SIZE_MAX) return &gpu.sets[gpu.current];
    for (size_t k = 0; k < FrameSlots; ++k) {
        size_t index = (gpu.next + k) % FrameSlots;
        auto& set = gpu.sets[index];
        if (set.pending || set.open) continue;
        if (!set.disjoint) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(gpu.device->CreateQuery(&desc, &set.disjoint))) return nullptr;
        }
        set.used = 0; set.open = true; set.generation = generation.load();
        { Issuing scope; gpu.context->Begin(set.disjoint.Get()); }
        gpu.current = index; gpu.next = index + 1;
        return &set;
    }
    // Every slot is still waiting for the GPU; this frame is not timed.
    std::lock_guard<std::mutex> guard(statsMutex); ++window.dropped;
    return nullptr;
}
void Record(int label, int kind)
{
    if (label < 0 || !gpu.context) return;
    auto set = OpenFrame(); if (!set) return;
    if (set->used >= MaxEntries) { std::lock_guard<std::mutex> guard(statsMutex); ++window.overflow; return; }
    if (set->used == set->queries.size()) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0}; ComPtr<ID3D11Query> query;
        if (FAILED(gpu.device->CreateQuery(&desc, &query))) return;
        set->queries.push_back(query); set->labels.push_back(0); set->kinds.push_back(0);
    }
    set->labels[set->used] = label; set->kinds[set->used] = kind;
    { Issuing scope; gpu.context->End(set->queries[set->used].Get()); }
    ++set->used;
}
// Reads finished frames in order without flushing or waiting.
void Resolve()
{
    while (!gpu.pending.empty()) {
        auto& set = gpu.sets[gpu.pending.front()];
        Issuing scope;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        HRESULT hr = gpu.context->GetData(set.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return;
        std::vector<UINT64> times(set.used);
        bool failed = FAILED(hr);
        for (size_t i = 0; i < set.used && !failed; ++i) {
            hr = set.queries[i] ? gpu.context->GetData(set.queries[i].Get(), &times[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) : E_FAIL;
            if (hr == S_FALSE) return;
            failed = FAILED(hr);
        }
        set.pending = false; gpu.pending.pop_front();
        if (failed || disjoint.Disjoint || !disjoint.Frequency || !set.used) { std::lock_guard<std::mutex> guard(statsMutex); ++window.discarded; continue; }
        ProcessFrame(set, times, disjoint.Frequency);
    }
}
void __stdcall ProfilerEvent(int event, void*)
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    const int label = event >> 2, kind = event & 3;
    std::lock_guard<std::mutex> guard(gpuMutex);
    if (kind <= Instant) Record(label, kind);
}
}

int ProfilerLabel(const char* name)
{
    if (!name) return -1;
    std::lock_guard<std::mutex> guard(labelMutex);
    auto found = ids.find(name);
    if (found != ids.end()) return found->second;
    if (names.size() >= (1u << 20)) return -1;
    int id = static_cast<int>(names.size());
    names.push_back(name); ids[name] = id;
    return id;
}
bool ProfilerEnabled() { return enabled.load(std::memory_order_relaxed); }
bool ProfilerIssuingQuery() { return issuing; }
void ProfilerBegin(int label) { if (!ProfilerEnabled()) return; std::lock_guard<std::mutex> guard(gpuMutex); Record(label, Begin); }
void ProfilerEnd(int label) { if (!ProfilerEnabled()) return; std::lock_guard<std::mutex> guard(gpuMutex); Record(label, End); }
void ProfilerMark(int label) { if (!ProfilerEnabled()) return; std::lock_guard<std::mutex> guard(gpuMutex); Record(label, Instant); }

void ProfilerPresent(ID3D11Device* device, bool bridge)
{
    if (bridge) bridgePresents = true;
    const bool on = ProfilerEnabled();
    if (!on && !bound.load()) return;
    static const int present = ProfilerLabel("Present");
    std::lock_guard<std::mutex> guard(gpuMutex);
    if (!on) { Release(); return; }
    if (!device) return;
    if (!gpu.device) {
        gpu.device = device; device->GetImmediateContext(&gpu.context); bound = true;
        Log("PROFILER_BOUND device=%p; GPU timestamps on the immediate context, report every %.0f s to profile.log", device, ReportSeconds);
    } else if (gpu.device.Get() != device) return;
    {
        std::lock_guard<std::mutex> stats(statsMutex); ++window.presents;
    }
    if (gpu.current != SIZE_MAX) {
        Record(present, Instant);
        auto& set = gpu.sets[gpu.current];
        { Issuing scope; gpu.context->End(set.disjoint.Get()); }
        set.open = false; set.pending = true; gpu.pending.push_back(gpu.current); gpu.current = SIZE_MAX;
    }
    Resolve();
    std::lock_guard<std::mutex> stats(statsMutex);
    if (Since(window.started) >= ReportSeconds) Report("periodic");
}
void ProfilerPresentChain(IDXGISwapChain* chain)
{
    if (!chain || (!ProfilerEnabled() && !bound.load())) return;
    // D3D12 swap chains (the bridge's display) have no D3D11 device.
    // The bridge closes its own frames after its D3D11 copies; Streamline also
    // routes its Present through this hook.
    if (bridgePresents.load()) return;
    ComPtr<ID3D11Device> device;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)))) ProfilerPresent(device.Get());
}
void ProfilerCpu(int label, double milliseconds)
{
    if (label < 0 || !ProfilerEnabled()) return;
    std::lock_guard<std::mutex> guard(statsMutex);
    auto& s = window.stats[label]; s.cpuSum += milliseconds; s.cpuMax = (std::max)(s.cpuMax, milliseconds); ++s.cpuCalls;
}
void ProfilerGpu(int label, double milliseconds)
{
    if (label < 0 || !ProfilerEnabled()) return;
    std::lock_guard<std::mutex> guard(statsMutex);
    auto& s = window.stats[label]; s.extSum += milliseconds; s.extMax = (std::max)(s.extMax, milliseconds); ++s.extCalls;
}

ProfileScope::ProfileScope(int id) : label(ProfilerEnabled() ? id : -1)
{
    if (label < 0) return;
    QueryPerformanceCounter(&started); ProfilerBegin(label);
}
ProfileScope::~ProfileScope()
{
    if (label < 0) return;
    ProfilerEnd(label);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    ProfilerCpu(label, 1000.0 * static_cast<double>(now.QuadPart - started.QuadPart) / static_cast<double>(frequency.QuadPart));
}
ProfileCpuScope::ProfileCpuScope(int id) : label(ProfilerEnabled() ? id : -1) { if (label >= 0) QueryPerformanceCounter(&started); }
ProfileCpuScope::~ProfileCpuScope()
{
    if (label < 0) return;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    ProfilerCpu(label, 1000.0 * static_cast<double>(now.QuadPart - started.QuadPart) / static_cast<double>(frequency.QuadPart));
}

extern "C" void __cdecl RiskOfRainUpscaling_ProfilerSetEnabled(int value)
{
    const bool on = value != 0;
    std::lock_guard<std::mutex> guard(statsMutex);
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    if (on == enabled.load()) return;
    if (!on) Report("profiling turned off");
    ++generation; window = Window{}; QueryPerformanceCounter(&window.started);
    enabled = on;
    Log("PROFILER_%s report=profile.log", on ? "ON" : "OFF");
    if (on) Write("=== PROFILING ON. Steps: GPU = D3D11 timestamps around each step (ms per frame in which it ran); [a -> b] = whole Unity camera segment between camera events (includes the game's own work).\r\n\r\n");
}
extern "C" int __cdecl RiskOfRainUpscaling_ProfilerRegister(const char* name) { return ProfilerLabel(name); }
extern "C" void __cdecl RiskOfRainUpscaling_ProfilerSetContext(const char* value)
{
    if (!value) return;
    std::lock_guard<std::mutex> guard(statsMutex);
    if (context == value) return;
    if (enabled.load()) Report("configuration changed");
    context = value; ++generation;
    window = Window{}; if (frequency.QuadPart) QueryPerformanceCounter(&window.started);
}
extern "C" void* __cdecl RiskOfRainUpscaling_ProfilerGetEvent() { return reinterpret_cast<void*>(&ProfilerEvent); }
