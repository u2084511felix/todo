// main.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <array>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cstdlib>
#include <ncurses.h>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <sqlite3.h>

static const std::string DB_PATH = "/var/lib/todo/todosql.db";
static int selectedIndex = 0;
static int viewMode = 0;  // 0 = current, 1 = completed
static std::string activeFilterCategory = "All";

static WINDOW* listWin = nullptr;

// Forward declarations of UI functions
static void drawUI();
static void addTaskOverlay();
static std::string editTaskOverlay(const std::string &currentText);
static void addCategoryOverlay();
static void listCategoriesOverlay();
static void completeTaskUI();
static void deleteTaskUI();
static void gotoItem(int itemNum);
static void setReminderOverlay();

// Returns the current Unix timestamp in seconds.
long long get_unix_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return timestamp;
}

// A simple ncurses function to get a string from the user.
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

// Draw wrapped text inside a window.
static int drawWrappedText(WINDOW* win, int startY, int startX, int width, const std::string& text) {
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

// A temporary structure for holding a task (with any reminder data) fetched from the DB.
struct DBTask {
    int id;
    long long created_at;
    long long completed_at;
    int completed;
    std::string task;
    std::string category;
    long long scheduled_time;
    int triggered;
    std::string notification_message;
};

// Initialize the database (create tables if they don’t exist).
void initDB() {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Can't open DB: " << sqlite3_errmsg(db) << "\n";
        return;
    }
    const char* sqlTasks =
        "CREATE TABLE IF NOT EXISTS tasks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "created_at INTEGER NOT NULL, "
        "completed_at INTEGER, "
        "completed INTEGER NOT NULL, "
        "task TEXT NOT NULL, "
        "category TEXT, "
        "notification_id INTEGER"
        ");";
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sqlTasks, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Error creating tasks table: " << errMsg << "\n";
        sqlite3_free(errMsg);
    }
    const char* sqlNotifications =
        "CREATE TABLE IF NOT EXISTS notifications ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "scheduled_time INTEGER NOT NULL, "
        "triggered INTEGER NOT NULL, "
        "message TEXT"
        ");";
    if (sqlite3_exec(db, sqlNotifications, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Error creating notifications table: " << errMsg << "\n";
        sqlite3_free(errMsg);
    }
    sqlite3_close(db);
}

// Fetch tasks from the database (using a LEFT JOIN to include reminder info).
std::vector<DBTask> fetchTasks(int completedFlag, const std::string &filterCategory) {
    std::vector<DBTask> tasks;
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return tasks;
    std::string sql =
        "SELECT t.id, t.task, t.completed, t.created_at, t.completed_at, t.category, "
        "n.scheduled_time, n.triggered, n.message "
        "FROM tasks t LEFT JOIN notifications n ON t.notification_id = n.id "
        "WHERE t.completed = ?";
    if (filterCategory != "All") { sql += " AND t.category = ?"; }
    sql += " ORDER BY t.created_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return tasks;
    }
    sqlite3_bind_int(stmt, 1, completedFlag);
    if (filterCategory != "All") { sqlite3_bind_text(stmt, 2, filterCategory.c_str(), -1, SQLITE_TRANSIENT); }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DBTask task;
        task.id = sqlite3_column_int(stmt, 0);
        task.task = (const char*)sqlite3_column_text(stmt, 1);
        task.completed = sqlite3_column_int(stmt, 2);
        task.created_at = sqlite3_column_int64(stmt, 3);
        task.completed_at = (sqlite3_column_type(stmt, 4) == SQLITE_NULL) ? 0 : sqlite3_column_int64(stmt, 4);
        task.category = (const char*)sqlite3_column_text(stmt, 5);
        if (sqlite3_column_type(stmt, 6) == SQLITE_NULL) {
            task.scheduled_time = 0;
            task.triggered = 0;
            task.notification_message = "";
        } else {
            task.scheduled_time = sqlite3_column_int64(stmt, 6);
            task.triggered = sqlite3_column_int(stmt, 7);
            task.notification_message = (const char*)sqlite3_column_text(stmt, 8);
        }
        tasks.push_back(task);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return tasks;
}

// --- Direct DB operations (each function immediately updates the DB) ---

// Insert a new task into the DB.
int addTask(const std::string &taskText, const std::string &category) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return -1;
    const char* sql = "INSERT INTO tasks (created_at, completed, task, category) VALUES (?, 0, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return -1; }
    long long now = get_unix_timestamp();
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, taskText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int newId = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE) ? newId : -1;
}

// Update a task’s text.
bool updateTaskText(int taskId, const std::string &newText) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sql = "UPDATE tasks SET task = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_text(stmt, 1, newText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, taskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// Update a task’s category.
bool updateTaskCategory(int taskId, const std::string &newCategory) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sql = "UPDATE tasks SET category = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_text(stmt, 1, newCategory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, taskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// Mark a task as completed.
bool markTaskCompleted(int taskId) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sql = "UPDATE tasks SET completed = 1, completed_at = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    long long now = get_unix_timestamp();
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int(stmt, 2, taskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// Delete a task.
bool removeTask(int taskId) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sql = "DELETE FROM tasks WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_int(stmt, 1, taskId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// Add a reminder for a task: insert into notifications and update the task’s notification_id.
bool addReminderToTask(int taskId, long long scheduledTime, const std::string &message) {
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) return false;
    const char* sqlInsert = "INSERT INTO notifications (scheduled_time, triggered, message) VALUES (?, 0, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlInsert, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_int64(stmt, 1, scheduledTime);
    sqlite3_bind_text(stmt, 2, message.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); return false; }
    int reminderId = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    const char* sqlUpdate = "UPDATE tasks SET notification_id = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sqlUpdate, -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return false; }
    sqlite3_bind_int(stmt, 1, reminderId);
    sqlite3_bind_int(stmt, 2, taskId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// --- UI functions ---

// Draw the list window using the current DB state.
static void drawListUI() {
    const char* colnames = (viewMode == 0) ? " Current Tasks " : " Completed Tasks ";
    const char* colcat   = " Category ";
    const char* coldates = (viewMode == 0) ? " Added on " : " Completed on ";
    const char* reminder = " Reminder ";
    werase(listWin);
    wbkgd(listWin, COLOR_PAIR(4));  // Apply to the whole stdscr
    wrefresh(listWin);
    box(listWin, 0, 0);





    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, "%s", colnames);
    int dateColumnPos     = getmaxx(listWin) - 18;
    int reminderColPos    = getmaxx(listWin) - 56;
    int categoryColumnPos = getmaxx(listWin) - 36;
    mvwprintw(listWin, 0, reminderColPos, "%s", reminder);
    mvwprintw(listWin, 0, categoryColumnPos, "%s", colcat);
    mvwprintw(listWin, 0, dateColumnPos, "%s", coldates);
    std::vector<DBTask> tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (tasks.empty()) { selectedIndex = 0; }
    else {
        if (selectedIndex >= (int)tasks.size()) selectedIndex = tasks.size() - 1;
        if (selectedIndex < 0) selectedIndex = 0;
    }
    int visibleLines = getmaxy(listWin) - 2;
    int scrollOffset = (selectedIndex >= visibleLines) ? selectedIndex - (visibleLines - 1) : 0;
    int currentY = 1;
    for (int idx = scrollOffset; idx < (int)tasks.size(); idx++) {
        if (currentY >= getmaxy(listWin) - 1) break;
        if (idx == selectedIndex)
            wattron(listWin, COLOR_PAIR(2));
        else
            wattron(listWin, COLOR_PAIR(3));
        mvwprintw(listWin, currentY, 2, "%-3d", idx + 1);
        mvwprintw(listWin, currentY, categoryColumnPos, "%-12s", tasks[idx].category.c_str());
        char bufDate[64];
        time_t tDate = (time_t)((viewMode == 0) ? tasks[idx].created_at : tasks[idx].completed_at);
        std::strftime(bufDate, sizeof(bufDate), "%Y-%m-%d %H:%M", std::localtime(&tDate));
        mvwprintw(listWin, currentY, dateColumnPos, "%s", bufDate);
        char bufReminder[64] = "";
        if (tasks[idx].scheduled_time != 0) {
            time_t tRem = (time_t)tasks[idx].scheduled_time;
            std::strftime(bufReminder, sizeof(bufReminder), "%Y-%m-%d %H:%M", std::localtime(&tRem));
        }
        mvwprintw(listWin, currentY, reminderColPos, "%s", bufReminder);
        int linesUsed = drawWrappedText(listWin, currentY, 6, reminderColPos - 7, tasks[idx].task);
        if (idx == selectedIndex)
            wattroff(listWin, COLOR_PAIR(2));
        else
            wattroff(listWin, COLOR_PAIR(3));
        currentY += linesUsed;
    }
    wnoutrefresh(stdscr);
    wnoutrefresh(listWin);
}

// Draw the overall UI.
static void drawUI() {
    wattron(stdscr, COLOR_PAIR(3));
    mvprintw(1, 2, "CLI TODO APP");
    mvprintw(2, 2, "View Mode: %s", (viewMode == 0 ? "Current" : "Completed"));
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=category, r=reminder, e=edit, #:filter, Tab=switch, q=exit");
    mvprintw(5, 2, "Nav: Up/Down, PgUp/PgDn, Home/End, Goto ':<num>'");
    mvprintw(6, 2, "Category Filter: %s                 ", activeFilterCategory.c_str());
    wattroff(stdscr, COLOR_PAIR(3));
    drawListUI();
    doupdate();
}

// Overlay to add a new task.
static void addTaskOverlay() {
    int overlayHeight = 7, overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2, overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Enter new task:");
    wrefresh(overlayWin);
    std::string taskText = ncursesGetString(overlayWin, 2, 2, 1024);
    if (!taskText.empty()) { addTask(taskText, ""); }
    delwin(overlayWin);
}

// Overlay to edit a task.
std::string editTaskOverlay(const std::string &currentText) {
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

// Overlay to update the category for the selected task.
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
        std::vector<DBTask> tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
        if (!tasks.empty() && selectedIndex < (int)tasks.size()) {
            updateTaskCategory(tasks[selectedIndex].id, newCat);
        }
    }
    delwin(overlayWin);
}

// Overlay listing categories (fetched from the DB) to set the filter.
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
    std::vector<std::string> catList; catList.push_back("All");
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
}

// Mark the selected task as completed.
static void completeTaskUI() {
    std::vector<DBTask> tasks = fetchTasks(0, activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    markTaskCompleted(tasks[selectedIndex].id);
}

// Edit the selected task.
void editTaskUI() {
    std::vector<DBTask> tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    int taskId = tasks[selectedIndex].id;
    std::string newText = editTaskOverlay(tasks[selectedIndex].task);
    if (!newText.empty()) { updateTaskText(taskId, newText); }
}

// Delete the selected task.
static void deleteTaskUI() {
    std::vector<DBTask> tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (tasks.empty() || selectedIndex >= (int)tasks.size()) return;
    removeTask(tasks[selectedIndex].id);
}

// Goto a specific task (1-based index).
static void gotoItem(int itemNum) {
    std::vector<DBTask> tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
    if (itemNum < 1 || itemNum > (int)tasks.size()) return;
    selectedIndex = itemNum - 1;
}

// Overlay to set a reminder for the selected task.
void setReminderOverlay() {
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
    long long scheduledTime = get_unix_timestamp() + offsetSeconds;
    std::vector<DBTask> tasks = fetchTasks(0, activeFilterCategory);
    if (!tasks.empty() && selectedIndex < (int)tasks.size()) {
        addReminderToTask(tasks[selectedIndex].id, scheduledTime, tasks[selectedIndex].task);
    }
    delwin(overlayWin);
}

int main() {
    initDB();
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


    // Change the background color of the entire window
    bkgd(COLOR_PAIR(4));  // Assuming pair 4 is defined for the background

    // Define a new color pair for the background
    init_pair(4, COLOR_BLUE, COLOR_WHITE);  // Example: White text on Blue background

    wbkgd(stdscr, COLOR_PAIR(4));  // Apply to the whole stdscr
    refresh();  // Refresh the screen to apply changes


    int listStartY = 8, listStartX = 2;
    int listHeight = LINES - listStartY - 2, listWidth  = COLS - 4;
    listWin = newwin(listHeight, listWidth, listStartY, listStartX);
    keypad(listWin, true);
    selectedIndex = 0;
    drawUI();
    while (true) {
        int ch = wgetch(stdscr);
        bool needRedraw = false;
        switch (ch) {
            case 'q':
                delwin(listWin);
                endwin();
                return 0;
            case KEY_UP:
                if (selectedIndex > 0) { selectedIndex--; needRedraw = true; }
                break;
            case KEY_DOWN: {
                auto tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
                if (selectedIndex < (int)tasks.size() - 1) { selectedIndex++; needRedraw = true; }
            } break;
            case KEY_HOME:
                if (selectedIndex != 0) { selectedIndex = 0; needRedraw = true; }
                break;
            case KEY_END: {
                auto tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
                if (!tasks.empty()) { selectedIndex = tasks.size() - 1; needRedraw = true; }
            } break;
            case KEY_PPAGE:
                selectedIndex = (selectedIndex > 10) ? selectedIndex - 10 : 0;
                needRedraw = true;
                break;
            case KEY_NPAGE: {
                auto tasks = fetchTasks((viewMode == 0 ? 0 : 1), activeFilterCategory);
                selectedIndex = (selectedIndex + 10 < (int)tasks.size()) ? selectedIndex + 10 : tasks.size() - 1;
                needRedraw = true;
            } break;
            case 'r':
                setReminderOverlay();
                needRedraw = true;
                break;
            case 'n':
                addTaskOverlay();
                needRedraw = true;
                break;
            case 'c':
                completeTaskUI();
                needRedraw = true;
                break;
            case 'd':
                deleteTaskUI();
                needRedraw = true;
                break;
            case 's':
                addCategoryOverlay();
                needRedraw = true;
                break;
            case '#':
                listCategoriesOverlay();
                needRedraw = true;
                break;
            case 'e':
                editTaskUI();
                needRedraw = true;
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
                    try { gotoItem(std::stoi(lineInput)); needRedraw = true; } catch (...) {}
                }
                mvprintw(LINES - 1, 0, "                                              ");
                clrtoeol();
            } break;
            case '\t':
                viewMode = 1 - viewMode;
                selectedIndex = 0;
                needRedraw = true;
                break;
            default:
                break;
        }
        if (needRedraw) { drawUI(); }
    }
    delwin(listWin);
    endwin();
    return 0;
}

