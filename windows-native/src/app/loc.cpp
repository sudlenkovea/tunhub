#include "loc.h"

#include <windows.h>

#include <map>

#include "tunhub/str.h"

namespace tunhub::app::loc {
namespace {

const std::map<std::string, std::string>& russian() {
    static const std::map<std::string, std::string> ru = {
        {"Tunnels", "Туннели"},
        {"Start", "Подключить"},
        {"Stop", "Отключить"},
        {"Stop all", "Отключить все"},
        {"Import…", "Импорт…"},
        {"Edit", "Изменить"},
        {"Delete", "Удалить"},
        {"Logs", "Логи"},
        {"Settings", "Настройки"},
        {"Check conflicts", "Проверить конфликты"},
        {"Name", "Имя"},
        {"Type", "Тип"},
        {"Status", "Состояние"},
        {"Traffic", "Трафик"},
        {"running", "работает"},
        {"degraded", "с ошибками"},
        {"starting…", "запуск…"},
        {"stopping…", "остановка…"},
        {"failed", "ошибка"},
        {"stopped", "остановлен"},
        {"Close", "Закрыть"},
        {"Cancel", "Отмена"},
        {"Save", "Сохранить"},
        {"OK", "ОК"},
        {"Yes", "Да"},
        {"No", "Нет"},
        {"Pause", "Пауза"},
        {"Copy all", "Копировать всё"},
        {"System component not running",
         "Системный компонент не запущен"},
        {"TunHub needs a background service to manage tunnels.",
         "TunHub нужна фоновая служба для управления туннелями."},
        {"Install system component", "Установить системный компонент"},
        {"Installing…", "Установка…"},
        {"The component was installed but isn't responding yet.",
         "Компонент установлен, но пока не отвечает."},
        {"Select a tunnel", "Выберите туннель"},
        {"Import configuration", "Импорт конфигурации"},
        {"WireGuard / AmneziaWG configs (*.conf)", "Конфиги WireGuard / AmneziaWG (*.conf)"},
        {"OpenVPN profiles (*.ovpn)", "Профили OpenVPN (*.ovpn)"},
        {"All files (*.*)", "Все файлы (*.*)"},
        {"Import failed", "Не удалось импортировать"},
        {"Imported", "Импортировано"},
        {"Delete this tunnel?", "Удалить этот туннель?"},
        {"This removes its configuration and stored keys.",
         "Будут удалены его конфигурация и сохранённые ключи."},
        {"Connection failed", "Не удалось подключиться"},
        {"Sign in", "Вход"},
        {"Username", "Логин"},
        {"Password", "Пароль"},
        {"One-time code", "Одноразовый код"},
        {"Save login and password", "Сохранить логин и пароль"},
        {"Connect", "Подключить"},
        {"Interface language", "Язык интерфейса"},
        {"Launch TunHub at login", "Запускать TunHub при входе в систему"},
        {"Kill switch (global)", "Kill switch (глобально)"},
        {"Log capture", "Сбор логов"},
        {"Normal", "Обычный"},
        {"Verbose (debug)", "Подробный (отладка)"},
        {"Verbose records every command and the tunnel core's debug output. Use it for "
         "troubleshooting only — it produces a lot of data and uses noticeably more CPU. "
         "Logs are kept in a single file, trimmed to the last 5 MB.",
         "Подробный режим записывает каждую команду и отладочный вывод ядра туннеля. "
         "Используйте только для диагностики — данных много и заметно растёт нагрузка на "
         "процессор. Логи хранятся в одном файле, обрезаемом до последних 5 МБ."},
        {"Restart required", "Требуется перезапуск"},
        {"The new log capture mode starts collecting after a restart. Restart TunHub now?",
         "Новый режим сбора логов начнёт работать после перезапуска. Перезапустить TunHub?"},
        {"Restart now", "Перезапустить"},
        {"Later", "Позже"},
        {"Open window", "Открыть окно"},
        {"Quit TunHub", "Выход из TunHub"},
        {"Disconnect all tunnels before quitting?",
         "Отключить все туннели перед выходом?"},
        {"Some tunnels are still connected.", "Некоторые туннели ещё подключены."},
        {"Disconnect and quit", "Отключить и выйти"},
        {"Quit, keep running", "Выйти, оставить включёнными"},
        {"No conflicts found", "Конфликтов не найдено"},
        {"Conflicts", "Конфликты"},
        {"Error", "Ошибка"},
        {"Warning", "Предупреждение"},
        {"Info", "Информация"},
        {"Interface", "Интерфейс"},
        {"Addresses", "Адреса"},
        {"DNS", "DNS"},
        {"MTU", "MTU"},
        {"Listen port", "Порт прослушивания"},
        {"Public key", "Публичный ключ"},
        {"Private key", "Приватный ключ"},
        {"Peers", "Пиры"},
        {"Endpoint", "Endpoint"},
        {"Allowed IPs", "Allowed IPs"},
        {"Keepalive", "Keepalive"},
        {"Obfuscation (AmneziaWG)", "Обфускация (AmneziaWG)"},
        {"Options", "Параметры"},
        {"Kill switch (block traffic outside the tunnel)",
         "Kill switch (блокировать трафик вне туннеля)"},
        {"Connect on app launch", "Подключать при запуске"},
        {"Uptime", "Время работы"},
        {"Received", "Принято"},
        {"Sent", "Отправлено"},
        {"Last handshake", "Последний handshake"},
        {"never", "не было"},
        {"Routes", "Маршруты"},
        {"all traffic (default route)", "весь трафик (маршрут по умолчанию)"},
    };
    return ru;
}

std::string g_language = "system";

bool useRussian() {
    if (g_language == "ru") return true;
    if (g_language == "en") return false;
    // "system": follow the UI language.
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
        return str::startsWith(str::lower(str::narrow(name)), "ru");
    return false;
}

}  // namespace

void setLanguage(const std::string& code) { g_language = code; }

const std::string& t(const std::string& key) {
    if (useRussian()) {
        const auto& table = russian();
        if (auto it = table.find(key); it != table.end()) return it->second;
    }
    return key;   // English source text doubles as the key
}

std::wstring w(const std::string& key) { return str::widen(t(key)); }

}  // namespace tunhub::app::loc
