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

// The Notification struct now includes task_id along with the id.
struct Notification {
    int id;
    int taskId;            // New field: the associated task's ID.
    long long scheduledTime;
    int triggered;         // 0 = not triggered, 1 = triggered (default 0)
    std::string message;
};

// Initialize the database: create the notifications table using the new schema,
// and enable WAL mode and foreign key support.
void initDB() {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Can't open DB: " << sqlite3_errmsg(db) << "\n";
        return;
    }
    // Enable Write-Ahead Logging and foreign key enforcement.
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    const char* sql =
        "CREATE TABLE IF NOT EXISTS notifications ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "task_id INTEGER NOT NULL, "                     // one-to-many relation with tasks
        "scheduled_time INTEGER NOT NULL, "
        "triggered INTEGER NOT NULL DEFAULT 0, "         // default value for triggered
        "message TEXT, "
        "FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE"
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
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK)
        return notifs;
    
    const char* sql = "SELECT id, task_id, scheduled_time, triggered, message FROM notifications WHERE triggered = 0 AND scheduled_time <= ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return notifs;
    }
    
    long long now = std::time(nullptr);
    sqlite3_bind_int64(stmt, 1, now);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Notification n;
        n.id = sqlite3_column_int(stmt, 0);
        n.taskId = sqlite3_column_int(stmt, 1);
        n.scheduledTime = sqlite3_column_int64(stmt, 2);
        n.triggered = sqlite3_column_int(stmt, 3);
        n.message = (const char*)sqlite3_column_text(stmt, 4);
        notifs.push_back(n);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return notifs;
}

// Mark a notification as triggered.
bool updateNotificationTriggered(int notifId) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK)
        return false;
    
    const char* sql = "UPDATE notifications SET triggered = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
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
            // Use a system command (e.g., notify-send) to display the notification.
            std::string command = "notify-send 'TODO!' '" + n.message + "'";
            system(command.c_str());
            updateNotificationTriggered(n.id);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

