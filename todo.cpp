// main.cpp
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include <ncurses.h>
#include <sqlite3.h>
#include <cctype>

// --------------------------------------------------------------------
// Data structure representing a task (with next upcoming notification).
// --------------------------------------------------------------------
struct DBTask {
    int id;
    long long created_at;
    long long updated_at;
    long long completed_at;
    int completed;  // 0 or 1
    std::string task;
    std::string category;
    // Next (earliest non-triggered) notification details (if any)
    long long scheduled_time;
    int triggered;  // 0 or 1
    std::string notification_message;
};

// --------------------------------------------------------------------
// DBManager: encapsulates the SQLite connection and common operations.
// --------------------------------------------------------------------
class DBManager {
public:
    DBManager(const std::string& dbPath) : db_(nullptr) {
        if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("Can't open DB: " + std::string(sqlite3_errmsg(db_)));
        }
        // Enable Write-Ahead Logging and foreign key support.
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
        initDB();
    }
    
    ~DBManager() {
        if (db_) sqlite3_close(db_);
    }
    
    // Create (or update) the tables and indexes.
    void initDB() {
        // Create tasks table.
        const char* sqlTasks = R"(
            CREATE TABLE IF NOT EXISTS tasks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                completed_at INTEGER,
                completed INTEGER NOT NULL DEFAULT 0,
                task TEXT NOT NULL,
                category TEXT DEFAULT ''
            );
        )";
        char* errMsg = nullptr;
        if (sqlite3_exec(db_, sqlTasks, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "Error creating tasks table: " << errMsg << "\n";
            sqlite3_free(errMsg);
        }
        
        // Create notifications table (one-to-many: a task can have several notifications).
        const char* sqlNotifications = R"(
            CREATE TABLE IF NOT EXISTS notifications (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                task_id INTEGER NOT NULL,
                scheduled_time INTEGER NOT NULL,
                triggered INTEGER NOT NULL DEFAULT 0,
                message TEXT,
                FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
            );
        )";
        if (sqlite3_exec(db_, sqlNotifications, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "Error creating notifications table: " << errMsg << "\n";
            sqlite3_free(errMsg);
        }
        
        // Create indexes.
        const char* idxTasks1 = "CREATE INDEX IF NOT EXISTS idx_tasks_completed_created_at ON tasks (completed, created_at);";
        sqlite3_exec(db_, idxTasks1, nullptr, nullptr, nullptr);
        const char* idxTasksCategory = "CREATE INDEX IF NOT EXISTS idx_tasks_category ON tasks (category);";
        sqlite3_exec(db_, idxTasksCategory, nullptr, nullptr, nullptr);
        const char* idxNotifsTask = "CREATE INDEX IF NOT EXISTS idx_notifications_task_id ON notifications (task_id);";
        sqlite3_exec(db_, idxNotifsTask, nullptr, nullptr, nullptr);
        const char* idxNotifsScheduled = "CREATE INDEX IF NOT EXISTS idx_notifications_scheduled ON notifications (scheduled_time, triggered);";
        sqlite3_exec(db_, idxNotifsScheduled, nullptr, nullptr, nullptr);
    }
    
    // Returns the current Unix timestamp.
    static long long getUnixTimestamp() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }
    
    // Insert a new task.
    int addTask(const std::string &taskText, const std::string &category) {
        const char* sql = "INSERT INTO tasks (created_at, updated_at, completed, task, category) VALUES (?, ?, 0, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return -1;
        }
        long long now = getUnixTimestamp();
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_text(stmt, 3, taskText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, category.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        int newId = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE) ? newId : -1;
    }
    
    // Update a task’s text and updated_at.
    bool updateTaskText(int taskId, const std::string &newText) {
        const char* sql = "UPDATE tasks SET task = ?, updated_at = ? WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { return false; }
        long long now = getUnixTimestamp();
        sqlite3_bind_text(stmt, 1, newText.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int(stmt, 3, taskId);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE);
    }
    
    // Update a task’s category and updated_at.
    bool updateTaskCategory(int taskId, const std::string &newCategory) {
        const char* sql = "UPDATE tasks SET category = ?, updated_at = ? WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { return false; }
        long long now = getUnixTimestamp();
        sqlite3_bind_text(stmt, 1, newCategory.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int(stmt, 3, taskId);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE);
    }
    
    // Mark a task as completed; update completed_at and updated_at.
    bool markTaskCompleted(int taskId) {
        const char* sql = "UPDATE tasks SET completed = 1, completed_at = ?, updated_at = ? WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { return false; }
        long long now = getUnixTimestamp();
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int(stmt, 3, taskId);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE);
    }
    
    // Delete a task (its notifications are removed via ON DELETE CASCADE).
    bool removeTask(int taskId) {
        const char* sql = "DELETE FROM tasks WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { return false; }
        sqlite3_bind_int(stmt, 1, taskId);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE);
    }
    
    // Add a reminder (notification) for a task.
    bool addReminderToTask(int taskId, long long scheduledTime, const std::string &message) {
        const char* sql = "INSERT INTO notifications (task_id, scheduled_time, triggered, message) VALUES (?, ?, 0, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { return false; }
        sqlite3_bind_int(stmt, 1, taskId);
        sqlite3_bind_int64(stmt, 2, scheduledTime);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE);
    }
    
    // Fetch tasks (with an optional category filter) joined with the next non-triggered notification.
    std::vector<DBTask> fetchTasks(int completedFlag, const std::string &filterCategory) {
        std::vector<DBTask> tasks;
        std::string sql = R"(
            SELECT t.id, t.task, t.completed, t.created_at, t.updated_at, t.completed_at, t.category,
                   n.scheduled_time, n.triggered, n.message
            FROM tasks t
            LEFT JOIN (
              SELECT n1.*
              FROM notifications n1
              JOIN (
                SELECT task_id, MIN(scheduled_time) AS min_time
                FROM notifications
                WHERE triggered = 0
                GROUP BY task_id
              ) n2 ON n1.task_id = n2.task_id AND n1.scheduled_time = n2.min_time
            ) n ON t.id = n.task_id
            WHERE t.completed = ?
        )";
        if (filterCategory != "All") { sql += " AND t.category = ?"; }
        sql += " ORDER BY t.created_at ASC;";
        
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return tasks;
        }
        sqlite3_bind_int(stmt, 1, completedFlag);
        if (filterCategory != "All") {
            sqlite3_bind_text(stmt, 2, filterCategory.c_str(), -1, SQLITE_TRANSIENT);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DBTask task;
            task.id = sqlite3_column_int(stmt, 0);
            task.task = (const char*)sqlite3_column_text(stmt, 1);
            task.completed = sqlite3_column_int(stmt, 2);
            task.created_at = sqlite3_column_int64(stmt, 3);
            task.updated_at = sqlite3_column_int64(stmt, 4);
            task.completed_at = (sqlite3_column_type(stmt, 5) == SQLITE_NULL) ? 0 : sqlite3_column_int64(stmt, 5);
            task.category = (const char*)sqlite3_column_text(stmt, 6);
            if (sqlite3_column_type(stmt, 7) == SQLITE_NULL) {
                task.scheduled_time = 0;
                task.triggered = 0;
                task.notification_message = "";
            } else {
                task.scheduled_time = sqlite3_column_int64(stmt, 7);
                task.triggered = sqlite3_column_int(stmt, 8);
                task.notification_message = (const char*)sqlite3_column_text(stmt, 9);
            }
            tasks.push_back(task);
        }
        sqlite3_finalize(stmt);
        return tasks;
    }
    
private:
    sqlite3* db_;
};

// --------------------------------------------------------------------
// Global variables for DB and UI state.
// --------------------------------------------------------------------
static const std::string DB_PATH = "/var/lib/todo/todosql.db";
DBManager* gDB = nullptr;
static int selectedIndex = 0;       // 0-based index in the cached list
static int viewMode = 0;            // 0 = current, 1 = completed
static std::string activeFilterCategory = "All";
static WINDOW* listWin = nullptr;

// Global cache of tasks and the current scroll offset.
std::vector<DBTask> gTasks;
int gScrollOffset = 0;  // computed based on visible area and task heights

// --------------------------------------------------------------------
// Helper: count how many lines will be used to wrap the given text.
// --------------------------------------------------------------------
int countWrappedLines(const std::string &text, int width) {
    if (text.empty()) return 1;
    int lineCount = 0;
    int len = text.size();
    int pos = 0;
    while (pos < len) {
        int end = pos + width;
        if (end > len) end = len;
        if (end < len) {
            int tmp = end;
            while (tmp > pos && !std::isspace((unsigned char)text[tmp])) { tmp--; }
            if (tmp == pos) { end = pos + width; }
            else { end = tmp; }
        }
        pos = end;
        while (pos < len && std::isspace((unsigned char)text[pos])) { pos++; }
        lineCount++;
    }
    return lineCount;
}

// --------------------------------------------------------------------
// Draw wrapped text inside a window (same as before).
// --------------------------------------------------------------------
int drawWrappedText(WINDOW* win, int startY, int startX, int width, const std::string& text) {
    if (text.empty()) { mvwprintw(win, startY, startX, ""); return 1; }
    int lineCount = 0;
    int len = text.size();
    int pos = 0;
    while (pos < len) {
        int end = pos + width;
        if (end > len) end = len;
        if (end < len) {
            int tmp = end;
            while (tmp > pos && !std::isspace((unsigned char)text[tmp])) { tmp--; }
            if (tmp == pos) { end = pos + width; }
            else { end = tmp; }
        }
        std::string buffer = text.substr(pos, end - pos);
        mvwprintw(win, startY + lineCount, startX, "%s", buffer.c_str());
        pos = end;
        while (pos < len && std::isspace((unsigned char)text[pos])) { pos++; }
        lineCount++;
    }
    return (lineCount > 0) ? lineCount : 1;
}

// --------------------------------------------------------------------
// Draw a single task (from gTasks) starting at y coordinate; returns number of lines used.
// --------------------------------------------------------------------
int drawTaskLine(int idx, int startY, bool highlight) {
    int maxX = getmaxx(listWin);
    int dateColumnPos     = maxX - 18;
    int reminderColPos    = maxX - 56;
    int categoryColumnPos = maxX - 36;
    
    if (highlight)
        wattron(listWin, COLOR_PAIR(2));
    else
        wattron(listWin, COLOR_PAIR(3));
    
    mvwprintw(listWin, startY, 2, "%-3d", idx + 1);
    mvwprintw(listWin, startY, categoryColumnPos, "%-12s", gTasks[idx].category.c_str());
    
    char bufDate[64];
    time_t tDate = (time_t)((viewMode == 0) ? gTasks[idx].created_at : gTasks[idx].completed_at);
    std::strftime(bufDate, sizeof(bufDate), "%Y-%m-%d %H:%M", std::localtime(&tDate));
    mvwprintw(listWin, startY, dateColumnPos, "%s", bufDate);
    
    char bufReminder[64] = "";
    if (gTasks[idx].scheduled_time != 0) {
        time_t tRem = (time_t)gTasks[idx].scheduled_time;
        std::strftime(bufReminder, sizeof(bufReminder), "%Y-%m-%d %H:%M", std::localtime(&tRem));
    }
    mvwprintw(listWin, startY, reminderColPos, "%s", bufReminder);
    
    // Draw the task text with wrapping.
    int linesUsed = drawWrappedText(listWin, startY, 6, reminderColPos - 7, gTasks[idx].task);
    
    if (highlight)
        wattroff(listWin, COLOR_PAIR(2));
    else
        wattroff(listWin, COLOR_PAIR(3));
    
    return linesUsed;
}

// --------------------------------------------------------------------
// Full redraw of the list window.
// --------------------------------------------------------------------
void drawListUIFull() {
    werase(listWin);
    wbkgd(listWin, COLOR_PAIR(4));
    box(listWin, 0, 0);
    
    int maxX = getmaxx(listWin);
    int reminderColPos = maxX - 56;
    // Draw header:
    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, (viewMode == 0 ? " Current Tasks " : " Completed Tasks "));
    int dateColumnPos     = maxX - 18;
    int categoryColumnPos = maxX - 36;
    mvwprintw(listWin, 0, reminderColPos, " Reminder ");
    mvwprintw(listWin, 0, categoryColumnPos, " Category ");
    mvwprintw(listWin, 0, dateColumnPos, (viewMode == 0 ? " Added on " : " Completed on "));
    
    // Update global task cache.
    gTasks = gDB->fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (gTasks.empty())
        selectedIndex = 0;
    else {
        if (selectedIndex >= (int)gTasks.size())
            selectedIndex = gTasks.size() - 1;
        if (selectedIndex < 0)
            selectedIndex = 0;
    }
    
    // Compute scroll offset based on cumulative line counts.
    int visibleLines = getmaxy(listWin) - 2;
    int cum = 0;
    gScrollOffset = 0;
    for (int i = 0; i < (int)gTasks.size(); i++) {
        int lines = countWrappedLines(gTasks[i].task, reminderColPos - 7);
        if (cum + lines >= visibleLines) {
            if (selectedIndex >= i) {
                gScrollOffset = i;
            }
            break;
        }
        cum += lines;
    }
    
    // Draw tasks starting from scroll offset.
    int currentY = 1;
    for (int idx = gScrollOffset; idx < (int)gTasks.size(); idx++) {
        if (currentY >= getmaxy(listWin) - 1)
            break;
        bool highlight = (idx == selectedIndex);
        int linesUsed = drawTaskLine(idx, currentY, highlight);
        currentY += linesUsed;
    }
    wnoutrefresh(listWin);
    doupdate();
}

// --------------------------------------------------------------------
// Compute the scroll offset required so that selectedIndex is visible.
// --------------------------------------------------------------------
int computeScrollOffsetForSelected() {
    int visibleLines = getmaxy(listWin) - 2;
    int offset = 0;
    int cum = 0;
    for (int i = 0; i < (int)gTasks.size(); i++) {
        int lines = countWrappedLines(gTasks[i].task, getmaxx(listWin) - 56 - 7);
        if (i == selectedIndex) {
            if (cum + lines > visibleLines) {
                // Adjust offset upward until selected fits.
                while (offset < selectedIndex && cum + lines > visibleLines) {
                    cum -= countWrappedLines(gTasks[offset].task, getmaxx(listWin) - 56 - 7);
                    offset++;
                }
            }
            break;
        }
        cum += lines;
        if (cum >= visibleLines) {
            offset = i + 1;
            cum = 0;
        }
    }
    return offset;
}

// --------------------------------------------------------------------
// Selectively update the two tasks (old and new selection) if they remain visible.
// Otherwise, perform a full redraw.
// --------------------------------------------------------------------
void updateSelectionDisplay(int oldSel, int newSel) {
    int newOffset = computeScrollOffsetForSelected();
    if (newOffset != gScrollOffset) {
        // The selected task has scrolled out of view; perform full redraw.
        drawListUIFull();
        return;
    }
    
    // Compute starting Y coordinate for a given task index (within visible region).
    int visibleLines = getmaxy(listWin) - 2;
    int y = 1;
    for (int i = gScrollOffset; i < oldSel; i++) {
        y += countWrappedLines(gTasks[i].task, getmaxx(listWin) - 56 - 7);
    }
    int oldY = y;
    // Redraw the old selected task without highlight.
    drawTaskLine(oldSel, oldY, false);
    
    y = 1;
    for (int i = gScrollOffset; i < newSel; i++) {
        y += countWrappedLines(gTasks[i].task, getmaxx(listWin) - 56 - 7);
    }
    int newY = y;
    // Redraw the new selected task with highlight.
    drawTaskLine(newSel, newY, true);
    
    wnoutrefresh(listWin);
    doupdate();
}

// --------------------------------------------------------------------
// Ncurses utility: get a string from a window (unchanged).
// --------------------------------------------------------------------
static std::string ncursesGetString(WINDOW* win, int startY, int startX, int maxLen = 1024, std::string result = "") {
    int ch;
    int cursorPos = result.size();
    wmove(win, startY, startX + cursorPos);
    wrefresh(win);
    curs_set(1);
    while (true) {
        ch = wgetch(win);
        if (ch == '\n' || ch == '\r') {
            break;
        } else if (ch == 27) { // ESC cancels editing
            result.clear();
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!result.empty() && cursorPos > 0) {
                result.erase(cursorPos - 1, 1);
                cursorPos--;
                mvwprintw(win, startY, startX, "%s ", result.c_str());
                wmove(win, startY, startX + cursorPos);
            }
        } else if (ch == KEY_LEFT) {
            if (cursorPos > 0) { cursorPos--; wmove(win, startY, startX + cursorPos); }
        } else if (ch == KEY_RIGHT) {
            if (cursorPos < (int)result.size()) { cursorPos++; wmove(win, startY, startX + cursorPos); }
        } else if (ch >= 32 && ch < 127) {
            if ((int)result.size() < maxLen) {
                result.insert(cursorPos, 1, static_cast<char>(ch));
                cursorPos++;
                mvwprintw(win, startY, startX, "%s ", result.c_str());
                wmove(win, startY, startX + cursorPos);
            }
        }
        wrefresh(win);
    }
    curs_set(0);
    return result;
}

// --------------------------------------------------------------------
// UI overlays for adding, editing, etc. (mostly unchanged)
// --------------------------------------------------------------------
static void addTaskOverlay() {
    int overlayHeight = 7, overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Enter new task:");
    wrefresh(overlayWin);
    std::string taskText = ncursesGetString(overlayWin, 2, 2, 1024);
    if (!taskText.empty()) {
        gDB->addTask(taskText, "");
    }
    delwin(overlayWin);
    drawListUIFull();
}

static std::string editTaskOverlay(const std::string &currentText) {
    int overlayHeight = 7, overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Edit task:");
    mvwprintw(overlayWin, 2, 2, "%s", currentText.c_str());
    wrefresh(overlayWin);
    std::string newText = ncursesGetString(overlayWin, 2, 2, 1024, currentText);
    delwin(overlayWin);
    return newText;
}

static void addCategoryOverlay() {
    int overlayHeight = 7, overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Enter new category:");
    wmove(overlayWin, 2, 2);
    wrefresh(overlayWin);
    std::string newCat = ncursesGetString(overlayWin, 2, 2, 1024);
    if (!newCat.empty()) {
        std::vector<DBTask> tasks = gDB->fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
        if (!tasks.empty() && selectedIndex < (int)tasks.size()) {
            gDB->updateTaskCategory(tasks[selectedIndex].id, newCat);
        }
    }
    delwin(overlayWin);
    drawListUIFull();
}

static void listCategoriesOverlay() {
    sqlite3* db = nullptr;
    std::set<std::string> uniqueCats;
    if (sqlite3_open(DB_PATH.c_str(), &db) == SQLITE_OK) {
        const char* sql = "SELECT DISTINCT category FROM tasks WHERE category IS NOT NULL AND category != '';";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* cat = (const char*)sqlite3_column_text(stmt, 0);
                if (cat) uniqueCats.insert(cat);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    std::vector<std::string> catList;
    catList.push_back("All");
    for (auto &c : uniqueCats) { catList.push_back(c); }
    int overlayHeight = 5 + catList.size();
    if (overlayHeight > LINES - 2) overlayHeight = LINES - 2;
    int overlayWidth = 40;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Select a category to filter:");
    wrefresh(overlayWin);
    int catSelected = 0;
    keypad(overlayWin, true);
    while (true) {
        for (int i = 0; i < (int)catList.size(); i++) {
            if (i + 3 >= overlayHeight - 1) break;
            if (i == catSelected) wattron(overlayWin, COLOR_PAIR(2));
            else wattroff(overlayWin, COLOR_PAIR(2));
            mvwprintw(overlayWin, i + 3, 2, "%s  ", catList[i].c_str());
        }
        wrefresh(overlayWin);
        int ch = wgetch(overlayWin);
        if (ch == KEY_UP) { if (catSelected > 0) catSelected--; }
        else if (ch == KEY_DOWN) { if (catSelected < (int)catList.size() - 1) catSelected++; }
        else if (ch == 'q' || ch == 27) { break; }
        else if (ch == '\n' || ch == '\r') { activeFilterCategory = catList[catSelected]; break; }
    }
    delwin(overlayWin);
    drawListUIFull();
}

static void completeTaskUI() {
    std::vector<DBTask> tasks = gDB->fetchTasks(0, activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    gDB->markTaskCompleted(tasks[selectedIndex].id);
    drawListUIFull();
}

static void editTaskUI() {
    std::vector<DBTask> tasks = gDB->fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    int taskId = tasks[selectedIndex].id;
    std::string newText = editTaskOverlay(tasks[selectedIndex].task);
    if (!newText.empty()) {
        gDB->updateTaskText(taskId, newText);
    }
    drawListUIFull();
}

static void deleteTaskUI() {
    std::vector<DBTask> tasks = gDB->fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    gDB->removeTask(tasks[selectedIndex].id);
    drawListUIFull();
}

static void gotoItem(int itemNum) {
    std::vector<DBTask> tasks = gDB->fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (itemNum < 1 || itemNum > (int)tasks.size()) return;
    selectedIndex = itemNum - 1;
    drawListUIFull();
}

static void setReminderOverlay() {
    int overlayHeight = 8, overlayWidth = 60;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Set reminder quantity (integer):");
    wmove(overlayWin, 2, 2);
    wrefresh(overlayWin);
    std::string qtyStr = ncursesGetString(overlayWin, 2, 2, 32);
    long long quantity = 0;
    try { quantity = std::stoll(qtyStr); } catch (...) { quantity = 0; }
    mvwprintw(overlayWin, 3, 2, "Choose unit: (s)econds, (m)inutes, (h)ours, (d)ays");
    wrefresh(overlayWin);
    int unitCh = wgetch(overlayWin);
    if (unitCh >= 'A' && unitCh <= 'Z') { unitCh = unitCh - 'A' + 'a'; }
    auto convertToSeconds = [](long long qty, char unit) -> long long {
        switch (unit) {
            case 's': return qty;
            case 'm': return qty * 60;
            case 'h': return qty * 3600;
            case 'd': return qty * 86400;
            default:  return qty;
        }
    };
    long long offsetSeconds = convertToSeconds(quantity, (char)unitCh);
    long long scheduledTime = DBManager::getUnixTimestamp() + offsetSeconds;
    std::vector<DBTask> tasks = gDB->fetchTasks(0, activeFilterCategory);
    if (!tasks.empty() && selectedIndex < (int)tasks.size()) {
        gDB->addReminderToTask(tasks[selectedIndex].id, scheduledTime, tasks[selectedIndex].task);
    }
    delwin(overlayWin);
    drawListUIFull();
}

// --------------------------------------------------------------------
// Draw overall UI header and list, then update screen.
// --------------------------------------------------------------------
static void drawUI() {
    wattron(stdscr, COLOR_PAIR(3));
    mvprintw(1, 2, "CLI TODO APP");
    mvprintw(2, 2, "View Mode: %s", (viewMode == 0 ? "Current" : "Completed"));
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=category, r=reminder, e=edit, #:filter, Tab=switch, q=exit");
    mvprintw(5, 2, "Nav: Up/Down, PgUp/PgDn, Home/End, Goto ':<num>'");
    mvprintw(6, 2, "Category Filter: %s                 ", activeFilterCategory.c_str());
    wattroff(stdscr, COLOR_PAIR(3));
    drawListUIFull();
}

// --------------------------------------------------------------------
// Main event loop.
// --------------------------------------------------------------------
int main() {
    try {
        gDB = new DBManager(DB_PATH);
    } catch (const std::exception& ex) {
        std::cerr << "Database error: " << ex.what() << "\n";
        return 1;
    }
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, true);
    curs_set(0);
    if (!has_colors()) {
        endwin();
        std::cerr << "Your terminal does not support color." << std::endl;
        return 1;
    }
    start_color();
    init_pair(1, COLOR_BLUE, COLOR_WHITE);
    init_pair(2, COLOR_WHITE, COLOR_BLACK);
    init_pair(3, COLOR_BLUE, COLOR_WHITE);
    init_pair(4, COLOR_BLUE, COLOR_WHITE);
    
    bkgd(COLOR_PAIR(4));
    wbkgd(stdscr, COLOR_PAIR(4));
    refresh();
    
    int listStartY = 8, listStartX = 2;
    int listHeight = LINES - listStartY - 2, listWidth  = COLS - 4;
    listWin = newwin(listHeight, listWidth, listStartY, listStartX);
    keypad(listWin, true);
    selectedIndex = 0;
    drawUI();
    
    int oldSelected = selectedIndex;
    while (true) {
        int ch = wgetch(stdscr);
        bool needFullRedraw = false;
        switch (ch) {
            case 'q':
                delwin(listWin);
                endwin();
                delete gDB;
                return 0;
            case KEY_UP:
                if (selectedIndex > 0) {
                    oldSelected = selectedIndex;
                    selectedIndex--;
                    updateSelectionDisplay(oldSelected, selectedIndex);
                }
                break;
            case KEY_DOWN:
                if (selectedIndex < (int)gTasks.size() - 1) {
                    oldSelected = selectedIndex;
                    selectedIndex++;
                    updateSelectionDisplay(oldSelected, selectedIndex);
                }
                break;
            case KEY_HOME:
                if (selectedIndex != 0) {
                    selectedIndex = 0;
                    needFullRedraw = true;
                }
                break;
            case KEY_END:
            {
                if (!gTasks.empty()) {
                    selectedIndex = gTasks.size() - 1;
                    needFullRedraw = true;
                }
            } break;
            case KEY_PPAGE:
                if (selectedIndex > 10) selectedIndex -= 10;
                else selectedIndex = 0;
                needFullRedraw = true;
                break;
            case KEY_NPAGE:
            {
                if (selectedIndex + 10 < (int)gTasks.size()) selectedIndex += 10;
                else selectedIndex = gTasks.size() - 1;
                needFullRedraw = true;
            } break;
            case 'r':
                setReminderOverlay();
                break;
            case 'n':
                addTaskOverlay();
                break;
            case 'c':
                completeTaskUI();
                break;
            case 'd':
                deleteTaskUI();
                break;
            case 's':
                addCategoryOverlay();
                break;
            case '#':
                listCategoriesOverlay();
                break;
            case 'e':
                editTaskUI();
                break;
            case ':': {
                mvprintw(LINES - 1, 0, "Goto item (blank=cancel): ");
                clrtoeol();
                echo();
                curs_set(1);
                char buffer[16];
                memset(buffer, 0, sizeof(buffer));
                wgetnstr(stdscr, buffer, 15);
                noecho();
                curs_set(0);
                std::string lineInput(buffer);
                if (!lineInput.empty()) {
                    try {
                        gotoItem(std::stoi(lineInput));
                    } catch (...) {}
                }
                mvprintw(LINES - 1, 0, "                                              ");
                clrtoeol();
            } break;
            case '\t':
                viewMode = 1 - viewMode;
                selectedIndex = 0;
                needFullRedraw = true;
                break;
            default:
                break;
        }
        if (needFullRedraw)
            drawUI();
    }
    
    delwin(listWin);
    endwin();
    delete gDB;
    return 0;
}

