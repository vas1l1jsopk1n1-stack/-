// main.cpp
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <mutex>
#include <atomic>
#include <chrono>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

// --- miniaudio: включаем dr_mp3 и dr_wav для поддержки форматов ---
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using json = nlohmann::json;

#define ID_SEARCH_BOX    101
#define ID_SOUND_LIST    102
#define ID_PLAY_BUTTON   103
#define ID_STATUS_BOX    104
#define ID_REFRESH_BUTTON 105   // <-- НОВОЕ

#define WM_APP_INDEX_LOADED  (WM_APP + 1)
#define WM_APP_INDEX_FAILED  (WM_APP + 2)

// -----------------------------------------------------------------------------
// Модель данных
// -----------------------------------------------------------------------------
struct SoundItem {
    int id;
    std::string title;
    std::string url;
};

// -----------------------------------------------------------------------------
// Глобальное состояние
// -----------------------------------------------------------------------------
std::vector<SoundItem> g_allSounds;
std::vector<SoundItem> g_filteredSounds;
std::mutex g_soundsMutex;

HWND hSearchBox     = NULL;
HWND hSoundList     = NULL;
HWND hPlayButton    = NULL;
HWND hStatusBox     = NULL;
HWND hRefreshButton = NULL;   // <-- НОВОЕ
HWND g_hWndMain     = NULL;

ma_context g_audioContext;
ma_engine  g_audioEngine;
ma_sound   g_currentSound;
std::atomic<bool> g_isSoundLoaded{ false };
std::atomic<bool> g_isEngineInitialized{ false };
std::atomic<bool> g_isLoading{ false };   // <-- НОВОЕ: защита от двойного клика
std::mutex g_audioMutex;

std::filesystem::path g_tempSessionDir;

// -----------------------------------------------------------------------------
// Кодировки
// -----------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), wstr.data(), size_needed);
    return wstr;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(),
                                          nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(),
                        str.data(), size_needed, nullptr, nullptr);
    return str;
}

// -----------------------------------------------------------------------------
// Логирование
// -----------------------------------------------------------------------------
void LogMessage(const std::wstring& msg) {
    OutputDebugStringW((L"[DEBUG] " + msg + L"\n").c_str());
    if (hStatusBox && IsWindow(hStatusBox)) {
        SetWindowTextW(hStatusBox, msg.c_str());
    }
}

// -----------------------------------------------------------------------------
// Cache-busting
// -----------------------------------------------------------------------------
std::string AddCacheBuster(const std::string& url) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count();
    std::string sep = (url.find('?') == std::string::npos) ? "?" : "&";
    return url + sep + "t=" + std::to_string(ms);
}

// -----------------------------------------------------------------------------
// Нормализация URL
// -----------------------------------------------------------------------------
std::string NormalizeToRawUrl(const std::string& input) {
    std::string url = input;
    if (url.rfind("https://raw.githubusercontent.com/", 0) == 0) return url;

    const std::string ghPrefix = "https://github.com/";
    if (url.rfind(ghPrefix, 0) == 0) {
        url.replace(0, ghPrefix.size(), "https://raw.githubusercontent.com/");
        auto pos = url.find("/blob/");
        if (pos != std::string::npos) url.replace(pos, 6, "/");
    }
    return url;
}

// -----------------------------------------------------------------------------
// Аудио-движок
// -----------------------------------------------------------------------------
bool InitAudioEngine() {
    LogMessage(L"Инициализация аудио-движка...");

    ma_result result = ma_context_init(nullptr, 0, nullptr, &g_audioContext);
    if (result != MA_SUCCESS) {
        LogMessage(L"Ошибка ma_context_init: " + std::to_wstring(result));
        return false;
    }

    ma_device_info* pPlaybackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    result = ma_context_get_devices(&g_audioContext, &pPlaybackInfos, &playbackCount,
                                    nullptr, nullptr);
    if (result != MA_SUCCESS || playbackCount == 0) {
        LogMessage(L"Аудиоустройства не найдены!");
        ma_context_uninit(&g_audioContext);
        return false;
    }

    ma_engine_config cfg = ma_engine_config_init();
    cfg.pContext = &g_audioContext;

    result = ma_engine_init(&cfg, &g_audioEngine);
    if (result != MA_SUCCESS) {
        LogMessage(L"Ошибка ma_engine_init: " + std::to_wstring(result));
        ma_context_uninit(&g_audioContext);
        return false;
    }

    g_isEngineInitialized = true;
    LogMessage(L"Аудио-движок запущен.");
    return true;
}

void ShutdownAudioEngine() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_isSoundLoaded) {
        ma_sound_stop(&g_currentSound);
        ma_sound_uninit(&g_currentSound);
        g_isSoundLoaded = false;
    }
    if (g_isEngineInitialized) {
        ma_engine_uninit(&g_audioEngine);
        ma_context_uninit(&g_audioContext);
        g_isEngineInitialized = false;
    }
}

// -----------------------------------------------------------------------------
// Детекторы форматов
// -----------------------------------------------------------------------------
bool LooksLikeMp3(const std::string& data) {
    if (data.size() < 4) return false;
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') return true;
    unsigned char b0 = static_cast<unsigned char>(data[0]);
    unsigned char b1 = static_cast<unsigned char>(data[1]);
    return b0 == 0xFF && (b1 & 0xE0) == 0xE0;
}

bool LooksLikeWav(const std::string& data) {
    return data.size() >= 12 &&
           data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
           data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E';
}

bool LooksLikeHtml(const std::string& data) {
    size_t probe = (std::min)(data.size(), static_cast<size_t>(512));
    std::string head = data.substr(0, probe);
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return head.find("<html") != std::string::npos ||
           head.find("<!doctype") != std::string::npos;
}

// -----------------------------------------------------------------------------
// Поиск / список
// -----------------------------------------------------------------------------
void UpdateList(HWND /*hwnd*/) {
    wchar_t queryBuf[256] = {};
    GetWindowTextW(hSearchBox, queryBuf, 256);
    std::wstring wquery = queryBuf;

    std::transform(wquery.begin(), wquery.end(), wquery.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });

    SendMessageW(hSoundList, LB_RESETCONTENT, 0, 0);

    std::lock_guard<std::mutex> lock(g_soundsMutex);
    g_filteredSounds.clear();

    for (const auto& sound : g_allSounds) {
        std::wstring wtitle = Utf8ToWide(sound.title);
        std::wstring wtitleLower = wtitle;
        std::transform(wtitleLower.begin(), wtitleLower.end(), wtitleLower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });

        if (wquery.empty() || wtitleLower.find(wquery) != std::wstring::npos) {
            g_filteredSounds.push_back(sound);
            SendMessageW(hSoundList, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(wtitle.c_str()));
        }
    }
}

// -----------------------------------------------------------------------------
// Загрузка каталога
// -----------------------------------------------------------------------------
void LoadGitHubIndex(HWND hwnd) {
    // Защита от параллельных загрузок
    if (g_isLoading.exchange(true)) {
        LogMessage(L"Загрузка уже идёт...");
        return;
    }

    LogMessage(L"Загрузка каталога...");

    // Блокируем кнопку обновления на время загрузки
    if (hRefreshButton && IsWindow(hRefreshButton)) {
        EnableWindow(hRefreshButton, FALSE);
        SetWindowTextW(hRefreshButton, L"Обновление...");
    }

    std::thread([hwnd]() {
        const std::string baseIndexUrl =
            "https://raw.githubusercontent.com/illiciti-hub/SopkinSpadLibrary/main/index.json";
        const std::string indexUrl = AddCacheBuster(baseIndexUrl);

        LogMessage(L"Запрос: " + Utf8ToWide(indexUrl));

        cpr::Header headers = {
            { "Cache-Control", "no-cache, no-store, must-revalidate" },
            { "Pragma",        "no-cache" },
            { "Expires",       "0" }
        };

        cpr::Response r = cpr::Get(
            cpr::Url{ indexUrl },
            headers,
            cpr::Timeout{ 15000 });

        if (r.status_code != 200) {
            std::wstring err = L"Ошибка скачивания JSON (HTTP " +
                               std::to_wstring(r.status_code) + L")";
            LogMessage(err);
            g_isLoading = false;
            PostMessageW(hwnd, WM_APP_INDEX_FAILED, 0, 0);
            return;
        }

        try {
            auto data = json::parse(r.text);
            std::vector<SoundItem> loaded;
            loaded.reserve(data.size());

            for (const auto& item : data) {
                SoundItem s;
                s.id    = item.value("id", 0);
                s.title = item.value("title", std::string("Untitled"));
                s.url   = NormalizeToRawUrl(item.value("url", std::string("")));
                loaded.push_back(std::move(s));
            }

            size_t count = loaded.size();
            {
                std::lock_guard<std::mutex> lock(g_soundsMutex);
                g_allSounds = std::move(loaded);
            }

            LogMessage(L"Каталог загружен. Звуков: " + std::to_wstring(count));
            g_isLoading = false;
            PostMessageW(hwnd, WM_APP_INDEX_LOADED, 0, 0);
        }
        catch (const std::exception& e) {
            LogMessage(L"Ошибка парсинга JSON: " + Utf8ToWide(e.what()));
            g_isLoading = false;
            PostMessageW(hwnd, WM_APP_INDEX_FAILED, 0, 0);
        }
    }).detach();
}

// -----------------------------------------------------------------------------
// Воспроизведение
// -----------------------------------------------------------------------------
void PlaySelectedSound() {
    int sel = static_cast<int>(SendMessageW(hSoundList, LB_GETCURSEL, 0, 0));

    SoundItem sound;
    {
        std::lock_guard<std::mutex> lock(g_soundsMutex);
        if (sel == LB_ERR || sel >= static_cast<int>(g_filteredSounds.size())) {
            LogMessage(L"Звук не выбран!");
            return;
        }
        sound = g_filteredSounds[sel];
    }

    if (!g_isEngineInitialized) {
        LogMessage(L"Аудио-движок не инициализирован!");
        return;
    }

    LogMessage(L"Скачивание: " + Utf8ToWide(sound.title));
    LogMessage(L"URL: " + Utf8ToWide(sound.url));

    std::thread([sound]() {
        std::string audioUrl = AddCacheBuster(sound.url);

        cpr::Response r = cpr::Get(
            cpr::Url{ audioUrl },
            cpr::Header{ { "Cache-Control", "no-cache" } },
            cpr::Timeout{ 30000 });

        if (r.status_code != 200) {
            std::wstring err = L"HTTP " + std::to_wstring(r.status_code) +
                               L"\nURL: " + Utf8ToWide(sound.url);
            MessageBoxW(g_hWndMain, err.c_str(), L"Ошибка сети",
                        MB_OK | MB_ICONERROR);
            return;
        }

        if (r.text.size() < 128) {
            std::wstring err = L"Файл слишком мал (" +
                               std::to_wstring(r.text.size()) + L" байт).";
            MessageBoxW(g_hWndMain, err.c_str(), L"Ошибка данных",
                        MB_OK | MB_ICONERROR);
            return;
        }

        if (LooksLikeHtml(r.text)) {
            std::wstring err =
                L"Сервер вернул HTML вместо аудио.\n"
                L"Скорее всего ссылка ведёт на страницу GitHub,\n"
                L"а не на raw-файл.\n\nURL: " + Utf8ToWide(sound.url);
            MessageBoxW(g_hWndMain, err.c_str(), L"Неверный формат",
                        MB_OK | MB_ICONERROR);
            LogMessage(L"Получен HTML вместо MP3");
            return;
        }

        bool isMp3 = LooksLikeMp3(r.text);
        bool isWav = LooksLikeWav(r.text);

        if (!isMp3 && !isWav) {
            std::wstring err = L"Файл не распознан как MP3 или WAV.\nПервые байты: ";
            for (int i = 0; i < 4 && i < static_cast<int>(r.text.size()); ++i) {
                wchar_t buf[8];
                swprintf_s(buf, L"%02X ", static_cast<unsigned char>(r.text[i]));
                err += buf;
            }
            MessageBoxW(g_hWndMain, err.c_str(), L"Неизвестный формат",
                        MB_OK | MB_ICONERROR);
            return;
        }

        std::filesystem::path tempPath =
            g_tempSessionDir / ("sound_" + std::to_string(sound.id) +
                                (isMp3 ? ".mp3" : ".wav"));

        {
            std::ofstream out(tempPath, std::ios::binary);
            if (!out) {
                MessageBoxW(g_hWndMain, L"Не удалось создать временный файл!",
                            L"Ошибка файла", MB_OK | MB_ICONERROR);
                return;
            }
            out.write(r.text.data(),
                      static_cast<std::streamsize>(r.text.size()));
        }

        {
            std::lock_guard<std::mutex> lock(g_audioMutex);
            if (g_isSoundLoaded) {
                ma_sound_stop(&g_currentSound);
                ma_sound_uninit(&g_currentSound);
                g_isSoundLoaded = false;
            }

            ma_result result = ma_sound_init_from_file(
                &g_audioEngine,
                tempPath.string().c_str(),
                0, nullptr, nullptr,
                &g_currentSound);

            if (result != MA_SUCCESS) {
                std::wstringstream ss;
                ss << L"Не удалось декодировать файл через miniaudio!\n\n"
                   << L"Код ошибки: " << result << L"\n"
                   << L"Путь: " << tempPath.wstring() << L"\n"
                   << L"Размер: " << r.text.size() << L" байт";
                MessageBoxW(g_hWndMain, ss.str().c_str(),
                            L"Критическая ошибка", MB_OK | MB_ICONERROR);
                LogMessage(L"Ошибка декодирования, код: " +
                           std::to_wstring(result));
                return;
            }

            g_isSoundLoaded = true;

            ma_result startResult = ma_sound_start(&g_currentSound);
            if (startResult == MA_SUCCESS) {
                LogMessage(L"Воспроизведение: " + Utf8ToWide(sound.title));
            } else {
                LogMessage(L"Ошибка запуска: " + std::to_wstring(startResult));
            }
        }
    }).detach();
}

// -----------------------------------------------------------------------------
// Оконная процедура
// -----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWndMain = hwnd;

        CreateWindowW(L"STATIC", L"Поиск:", WS_VISIBLE | WS_CHILD,
                      10, 15, 50, 20, hwnd, nullptr, nullptr, nullptr);

        hSearchBox = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            65, 12, 240, 22, hwnd,           // <-- сжимаем поле поиска
            reinterpret_cast<HMENU>(ID_SEARCH_BOX), nullptr, nullptr);

        // <-- НОВОЕ: кнопка обновления справа от поля поиска
        hRefreshButton = CreateWindowW(L"BUTTON", L"⟳",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            310, 12, 55, 22, hwnd,
            reinterpret_cast<HMENU>(ID_REFRESH_BUTTON), nullptr, nullptr);

        hSoundList = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
            10, 45, 355, 180, hwnd,
            reinterpret_cast<HMENU>(ID_SOUND_LIST), nullptr, nullptr);

        hPlayButton = CreateWindowW(L"BUTTON", L"Воспроизвести (MP3/WAV)",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            10, 232, 355, 30, hwnd,
            reinterpret_cast<HMENU>(ID_PLAY_BUTTON), nullptr, nullptr);

        hStatusBox = CreateWindowW(L"STATIC", L"Инициализация...",
            WS_VISIBLE | WS_CHILD | SS_LEFTNOWORDWRAP,
            10, 268, 355, 20, hwnd,
            reinterpret_cast<HMENU>(ID_STATUS_BOX), nullptr, nullptr);

        try {
            g_tempSessionDir = std::filesystem::temp_directory_path() /
                               ("NativeSoundApp_" + std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(g_tempSessionDir);
        } catch (...) {
            g_tempSessionDir = std::filesystem::temp_directory_path();
        }

        InitAudioEngine();
        LoadGitHubIndex(hwnd);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_SEARCH_BOX && HIWORD(wParam) == EN_CHANGE) {
            UpdateList(hwnd);
        }
        else if (LOWORD(wParam) == ID_PLAY_BUTTON) {
            PlaySelectedSound();
        }
        else if (LOWORD(wParam) == ID_SOUND_LIST && HIWORD(wParam) == LBN_DBLCLK) {
            PlaySelectedSound();
        }
        // <-- НОВОЕ: обработка кнопки обновления
        else if (LOWORD(wParam) == ID_REFRESH_BUTTON) {
            LoadGitHubIndex(hwnd);
        }
        break;

    case WM_APP_INDEX_LOADED:
        // Восстанавливаем кнопку
        if (hRefreshButton && IsWindow(hRefreshButton)) {
            EnableWindow(hRefreshButton, TRUE);
            SetWindowTextW(hRefreshButton, L"⟳");
        }
        UpdateList(hwnd);
        break;

    case WM_APP_INDEX_FAILED:
        if (hRefreshButton && IsWindow(hRefreshButton)) {
            EnableWindow(hRefreshButton, TRUE);
            SetWindowTextW(hRefreshButton, L"⟳");
        }
        MessageBoxW(hwnd, L"Не удалось загрузить каталог звуков с GitHub.",
                    L"Ошибка", MB_OK | MB_ICONWARNING);
        break;

    case WM_DESTROY: {
        ShutdownAudioEngine();

        try {
            if (!g_tempSessionDir.empty() &&
                std::filesystem::exists(g_tempSessionDir)) {
                std::filesystem::remove_all(g_tempSessionDir);
            }
        } catch (...) {}

        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Точка входа
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SoundAppClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        L"SoundAppClass", L"GitHub Sound Player",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 390, 330,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Не удалось создать окно!", L"Ошибка",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(L"SoundAppClass", hInstance);
    return 0;
}