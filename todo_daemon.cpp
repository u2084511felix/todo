// daemon.cpp
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <sqlite3.h>

static const std::string DB_PATH = "/var/lib/todo/todosql.db";

// Extend the Notification struct to include an ID.
struct Notification {
    int id;
    long long scheduledTime;
    int triggered;
    std::string message;
};

// Initialize the database (ensure the notifications table exists).
void initDB() {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Can't open DB: " << sqlite3_errmsg(db) << "\n";
        return;
    }
    const char* sql =
        "CREATE TABLE IF NOT EXISTS notifications ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "scheduled_time INTEGER NOT NULL, "
        "triggered INTEGER NOT NULL, "
        "message TEXT"
        ");";
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Error creating notifications table: " << errMsg << "\n";
        sqlite3_free(errMsg);
    }
    sqlite3_close(db);
}

// Fetch pending notifications (those not yet triggered and whose scheduled time has passed).
std::vector<Notification> fetchPendingNotifications() {
    std::vector<Notification> notifs;
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return notifs;
    const char* sql = "SELECT id, scheduled_time, triggered, message FROM notifications WHERE triggered = 0 AND scheduled_time <= ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return notifs; }
    long long now = std::time(nullptr);
    sqlite3_bind_int64(stmt, 1, now);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Notification n;
        n.id = sqlite3_column_int(stmt, 0);
        n.scheduledTime = sqlite3_column_int64(stmt, 1);
        n.triggered = sqlite3_column_int(stmt, 2);
        n.message = (const char*)sqlite3_column_text(stmt, 3);
        notifs.push_back(n);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return notifs;
}

// Mark a notification as triggered.
bool updateNotificationTriggered(int notifId) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sql = "UPDATE notifications SET triggered = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_int(stmt, 1, notifId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

int main() {
    std::cout << "Notification daemon started. Monitoring scheduled notifications...\n";
    initDB();
    while (true) {
        auto notifs = fetchPendingNotifications();
        for (auto &n : notifs) {
            // Use a system command (e.g., notify-send) to show the notification.
            std::string command = "notify-send 'TODO!' '" + n.message + "'";
            system(command.c_str());
            updateNotificationTriggered(n.id);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

