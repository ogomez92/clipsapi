// Sapi2Prism: the voice that sends what programs say to the running screen
// reader (NVDA, JAWS, ZDSR, PC-Talker, Narrator through UI Automation, ...)
// through Prism (https://github.com/ethindp/prism, MPL-2.0).
//
// prism.dll is loaded on first use from the folder of this DLL, so Clipboard
// Voice keeps working where it is missing. All Prism calls run on one worker
// thread: a Prism backend is not thread-safe, and screen reader calls are
// inter-process calls that must not hold up SAPI's thread.
//
// Prism's own TTS backends (SAPI, OneCore) are never used: SAPI could pick this
// very voice and loop, and a voice talking over the screen reader is not what
// this voice is for. With no screen reader running the voice stays silent.
//
// Set the environment variable SAPI2PRISM_LOG to a file path to log what the
// voice does (backend chosen, every text sent and its result).

#include "ClipboardVoiceEngine.h"
// prism.h declares an enum value of 1 << 63, which MSVC warns about in C++.
#pragma warning(push)
#pragma warning(disable : 4309)
#include "prism.h"
#pragma warning(pop)

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

// ---------- Optional log ----------

std::mutex g_logMutex;

const std::wstring& LogPath()
{
    static const std::wstring path = [] {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"SAPI2PRISM_LOG", buf, ARRAYSIZE(buf));
        return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
    }();
    return path;
}

void Log(const char* fmt, ...)
{
    if (LogPath().empty()) return;
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = nullptr;
    if (_wfopen_s(&f, LogPath().c_str(), L"ab") == 0 && f)
    {
        fprintf(f, "%02u:%02u:%02u.%03u [%lu] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                ::GetCurrentProcessId(), msg);
        fclose(f);
    }
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    // Unpaired surrogates become U+FFFD, so the result is always valid UTF-8.
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ---------- prism.dll ----------

struct PrismApi
{
    decltype(&prism_init) init = nullptr;
    decltype(&prism_registry_count) registry_count = nullptr;
    decltype(&prism_registry_id_at) registry_id_at = nullptr;
    decltype(&prism_registry_name) registry_name = nullptr;
    decltype(&prism_registry_create) registry_create = nullptr;
    decltype(&prism_backend_free) backend_free = nullptr;
    decltype(&prism_backend_name) backend_name = nullptr;
    decltype(&prism_backend_get_features) backend_get_features = nullptr;
    decltype(&prism_backend_initialize) backend_initialize = nullptr;
    decltype(&prism_backend_speak) backend_speak = nullptr;
    decltype(&prism_backend_braille) backend_braille = nullptr;
    decltype(&prism_backend_stop) backend_stop = nullptr;
    decltype(&prism_backend_is_speaking) backend_is_speaking = nullptr;
    decltype(&prism_error_string) error_string = nullptr;
    decltype(&prism_version_string) version_string = nullptr;

    bool Load(HMODULE h)
    {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::GetProcAddress(h, name));
            if (!fn) { Log("prism.dll has no %s", name); ok = false; }
        };
        get(init, "prism_init");
        get(registry_count, "prism_registry_count");
        get(registry_id_at, "prism_registry_id_at");
        get(registry_name, "prism_registry_name");
        get(registry_create, "prism_registry_create");
        get(backend_free, "prism_backend_free");
        get(backend_name, "prism_backend_name");
        get(backend_get_features, "prism_backend_get_features");
        get(backend_initialize, "prism_backend_initialize");
        get(backend_speak, "prism_backend_speak");
        get(backend_braille, "prism_backend_braille");
        get(backend_stop, "prism_backend_stop");
        get(backend_is_speaking, "prism_backend_is_speaking");
        get(error_string, "prism_error_string");
        get(version_string, "prism_version_string");
        return ok;
    }

    const char* Err(PrismError e) const
    {
        const char* s = error_string ? error_string(e) : nullptr;
        return s ? s : "?";
    }
};

// How often to look for a (better) screen reader while speaking.
const ULONGLONG kRedetectMs = 1000;

// Owns Prism and the chosen backend; everything runs on its own thread.
class PrismService
{
public:
    static PrismService& Get()
    {
        // Never destroyed: the worker lives as long as the process.
        static PrismService* s = new PrismService();
        return *s;
    }

    // interrupt: cut what the screen reader is saying first.
    void Say(const std::wstring& text, bool interrupt)
    {
        if (interrupt) m_generation.fetch_add(1); // queued texts not sent yet are dropped
        const unsigned gen = m_generation.load();
        Post([this, utf8 = ToUtf8(text), interrupt, gen] {
            if (gen != m_generation.load()) return;
            DoSay(utf8, interrupt);
        });
    }

    void Stop()
    {
        m_generation.fetch_add(1);
        Post([this] { DoStop(); });
    }

    // 1 speaking, 0 silent, -1 unknown (no screen reader, or it can't tell, or no answer in time).
    int IsSpeaking(DWORD timeoutMs)
    {
        auto answer = std::make_shared<std::promise<int>>();
        std::future<int> result = answer->get_future();
        Post([this, answer] { answer->set_value(DoIsSpeaking()); });
        if (result.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
            return -1;
        return result.get();
    }

private:
    PrismService() = default;

    void Post(std::function<void()> task)
    {
        std::call_once(m_startOnce, [this] {
            // The worker runs code from this DLL for the rest of the process, so
            // pin the DLL (see StartWorkerIfNeeded in ClipboardVoiceEngine.cpp).
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                 reinterpret_cast<LPCWSTR>(&LogPath), &self);
            std::thread([this] { Run(); }).detach();
        });
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    void Run()
    {
        // JAWS, ZoomText and UI Automation are reached through COM.
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_tasks.empty(); });
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            task();
        }
    }

    // ----- worker thread only below -----

    bool EnsureLoaded()
    {
        if (m_ctx) return true;
        if (m_loadFailed) return false;
        m_loadFailed = true; // only try once per process

        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&LogPath), &self);
        wchar_t path[MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameW(self, path, ARRAYSIZE(path));
        if (n == 0 || n >= ARRAYSIZE(path)) return false;
        std::wstring dllPath(path, n);
        dllPath.erase(dllPath.find_last_of(L"\\/") + 1);
        dllPath += L"prism.dll";

        // Full path: a bare name could pick up another program's copy of another version.
        HMODULE h = ::LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h)
        {
            Log("could not load %ls (error %lu); Sapi2Prism stays silent", dllPath.c_str(), ::GetLastError());
            return false;
        }
        if (!m_api.Load(h)) return false;
        m_ctx = m_api.init(nullptr);
        if (!m_ctx)
        {
            Log("prism_init failed");
            return false;
        }
        m_loadFailed = false;
        Log("Prism %s loaded from %ls", m_api.version_string(), dllPath.c_str());
        return true;
    }

    static bool IsExcluded(PrismBackendId id)
    {
        // Prism's own voices: see the top of this file.
        return id == PRISM_BACKEND_SAPI || id == PRISM_BACKEND_ONE_CORE;
    }

    void DropBackend()
    {
        if (m_backend) m_api.backend_free(m_backend);
        m_backend = nullptr;
        m_backendId = PRISM_BACKEND_INVALID;
        m_features = 0;
    }

    // Picks the highest-priority screen reader that is running. Keeps the
    // current one when it is still the best, unless `reconnect` is set (its
    // connection broke, e.g. NVDA was restarted).
    void Detect(bool reconnect)
    {
        const ULONGLONG now = ::GetTickCount64();
        if (!reconnect && m_lastDetect && now - m_lastDetect < kRedetectMs) return;
        m_lastDetect = now;
        if (!EnsureLoaded()) return;

        const size_t count = m_api.registry_count(m_ctx);
        for (size_t i = 0; i < count; ++i)
        {
            const PrismBackendId id = m_api.registry_id_at(m_ctx, i);
            if (id == PRISM_BACKEND_INVALID || IsExcluded(id)) continue;

            // Uninitialised instances, kept to ask "is it running?" cheaply.
            PrismBackend*& probe = m_probes[id];
            if (!probe) probe = m_api.registry_create(m_ctx, id);
            if (!probe) continue;
            if (!(m_api.backend_get_features(probe) & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME)) continue;

            if (id == m_backendId && m_backend && !reconnect) return;

            PrismBackend* b = m_api.registry_create(m_ctx, id);
            if (!b) continue;
            const PrismError e = m_api.backend_initialize(b);
            if (e != PRISM_OK)
            {
                Log("%s is running but could not be started: %s", m_api.registry_name(m_ctx, id), m_api.Err(e));
                m_api.backend_free(b);
                continue;
            }
            DropBackend();
            m_backend = b;
            m_backendId = id;
            m_features = m_api.backend_get_features(b);
            Log("speaking through %s%s", m_api.backend_name(b),
                (m_features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING) ? " (it reports when it is speaking)" : "");
            return;
        }
        if (m_backend) Log("%s is no longer running", m_api.backend_name(m_backend));
        else if (!m_reportedNone) Log("no screen reader is running");
        m_reportedNone = true;
        DropBackend();
    }

    void DoSay(const std::string& utf8, bool interrupt)
    {
        Detect(false);
        if (!m_backend) return;
        PrismError e = m_api.backend_speak(m_backend, utf8.c_str(), interrupt);
        if (e != PRISM_OK && e != PRISM_ERROR_INVALID_UTF8)
        {
            // Speech failed before anything was said; reconnect and try once more.
            Log("%s failed (%s); reconnecting", m_api.backend_name(m_backend), m_api.Err(e));
            Detect(true);
            if (!m_backend) return;
            e = m_api.backend_speak(m_backend, utf8.c_str(), interrupt);
        }
        Log("say%s \"%s\": %s", interrupt ? " (interrupt)" : "", utf8.c_str(), m_api.Err(e));
        if (e == PRISM_OK && (m_features & PRISM_BACKEND_SUPPORTS_BRAILLE))
            (void)m_api.backend_braille(m_backend, utf8.c_str()); // best effort
    }

    void DoStop()
    {
        if (!m_backend || !(m_features & PRISM_BACKEND_SUPPORTS_STOP)) return;
        const PrismError e = m_api.backend_stop(m_backend);
        Log("stop: %s", m_api.Err(e));
    }

    int DoIsSpeaking()
    {
        if (!m_backend || !(m_features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return -1;
        bool speaking = false;
        if (m_api.backend_is_speaking(m_backend, &speaking) != PRISM_OK) return -1;
        return speaking ? 1 : 0;
    }

    std::once_flag m_startOnce;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    std::atomic<unsigned> m_generation{ 0 };

    // Worker thread state.
    PrismApi m_api;
    PrismContext* m_ctx = nullptr;
    bool m_loadFailed = false;
    std::map<PrismBackendId, PrismBackend*> m_probes;
    PrismBackend* m_backend = nullptr;
    PrismBackendId m_backendId = PRISM_BACKEND_INVALID;
    uint64_t m_features = 0;
    ULONGLONG m_lastDetect = 0;
    bool m_reportedNone = false;
};

// ---------- Interrupt or queue ----------
//
// SAPI does not tell the engine whether the program asked to purge: a purge
// that cuts an utterance in progress arrives as an abort (we stop the screen
// reader), and the purge flag never reaches the engine. So a text that follows
// right after the previous one finished normally is the next item of SAPI's
// queue and is queued after it; any other text (the first one, one after a
// pause, one after an interruption) interrupts what the screen reader says.
const ULONGLONG kQueuedFollowUpMs = 150;
std::atomic<ULONGLONG> g_lastDoneTick{ 0 };
std::atomic<bool> g_lastCompleted{ false };

// Hold the utterance while the screen reader speaks: wait this long for it to
// start, then poll; never longer than kMaxHoldBaseMs + kMaxHoldPerCharMs per char.
const ULONGLONG kStartWaitMs = 500;
const ULONGLONG kPollMs = 50;
const ULONGLONG kMaxHoldBaseMs = 2000;
const ULONGLONG kMaxHoldPerCharMs = 120;
const ULONGLONG kMaxHoldMs = 120000;

} // namespace

// ---------- Sapi2Prism voice ----------

void CPrismVoiceEngine::OnText(const std::wstring& text, bool purge)
{
    if (IsBlank(text))
    {
        // An empty purge request is "stop talking".
        if (purge) PrismService::Get().Stop();
        return;
    }
    const ULONGLONG now = ::GetTickCount64();
    const bool followUp = g_lastCompleted.load() && now - g_lastDoneTick.load() < kQueuedFollowUpMs;
    PrismService::Get().Say(text, purge || !followUp);
}

void CPrismVoiceEngine::OnAbort()
{
    PrismService::Get().Stop();
}

bool CPrismVoiceEngine::HoldWhileBusy(const std::wstring& text, ISpTTSEngineSite* pSite,
                                      const WAVEFORMATEX* pFmt, ULONGLONG& audioBytes)
{
    if (IsBlank(text)) return true;
    const ULONGLONG start = ::GetTickCount64();
    ULONGLONG maxMs = kMaxHoldBaseMs + kMaxHoldPerCharMs * text.size();
    if (maxMs > kMaxHoldMs) maxMs = kMaxHoldMs;
    const size_t chunk = (size_t)(pFmt->nAvgBytesPerSec * kPollMs / 1000);
    bool started = false;
    for (;;)
    {
        const ULONGLONG pollStart = ::GetTickCount64();
        const int speaking = PrismService::Get().IsSpeaking((DWORD)kPollMs * 4);
        if (speaking < 0) return true; // the screen reader can't tell: the usual pacing applies
        const ULONGLONG elapsed = pollStart - start;
        if (speaking == 1) started = true;
        if ((speaking == 0 && (started || elapsed >= kStartWaitMs)) || elapsed >= maxMs)
        {
            Log("held %llu ms for the screen reader%s", elapsed,
                started ? "" : " (it never reported speaking)");
            return true;
        }
        if (!WriteSilence(pSite, pFmt, chunk, audioBytes)) return false;
        // Writes return at once while SAPI buffers audio; poll at a steady pace anyway.
        const ULONGLONG spent = ::GetTickCount64() - pollStart;
        if (spent < kPollMs) ::Sleep((DWORD)(kPollMs - spent));
        if (Aborted(pSite)) return false;
    }
}

void CPrismVoiceEngine::OnDone(bool completed)
{
    g_lastCompleted.store(completed);
    g_lastDoneTick.store(::GetTickCount64());
}
