#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <set>
#include <cctype>
#include <wchar.h>
#include <algorithm>
#include <ncurses.h>
#include <ctime>
#include <filesystem>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
namespace fs = std::filesystem;

static const std::string NOTIFICATION_FILE = "/var/lib/todo/notifications.db";
static const std::string TODO_FILE = "/var/lib/todo/todo.db";

// Represents a single notification
struct Notification {
    long long scheduledTime;  // Unix epoch when the notification should fire
    bool triggered;           // Has the notification been triggered already?
    std::string message;      // The text of the notification
};

struct Task {
    std::string task;
    bool completed;
    std::string category;
    // We'll store exactly two timestamps:
    //   dates[0] = creation time
    //   dates[1] = completion time (if completed)
    std::array<long long, 2> dates;
    Notification notification; 
};

static std::vector<Notification> notifications;
static std::vector<Task> allTasks;
static std::vector<Task> currentTasks;
static std::vector<Task> completedTasks;

static int selectedIndex = 0;
static int viewMode = 0;  // 0 = current, 1 = completed

// This category filter determines which items we display.
// "All" means no filter; otherwise, show only tasks with matching category.
static std::string activeFilterCategory = "All";

static WINDOW* listWin = nullptr;

// Forward declarations
static void drawUI();
static void addTaskOverlay();
static void addCategoryOverlay(int taskIndex, bool forCompleted);
static void listCategoriesOverlay();
static void completeTask();
static void deleteTask();
static void gotoItem(int itemNum);
static Notification setReminderOverlay();

long long convertToSeconds(long long quantity, char unit) {
    switch (unit) {
        case 's': return quantity;
        case 'm': return quantity * 60;
        case 'h': return quantity * 3600;
        case 'd': return quantity * 86400;
        default:  return quantity; // fallback to seconds
    }
}

long long get_unix_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return timestamp;
}


static std::string ncursesGetString(WINDOW* win, int startY, int startX, int maxLen = 1024, std::string result = "") {
    wchar_t ch;
    int cursorPos = result.size();  // Track cursor position within string
    wmove(win, startY, startX + cursorPos);
    wrefresh(win);
    curs_set(1);  // Show cursor

    while (true) {
        ch = getch();

        if (ch == '\n' || ch == '\r') {
            // Enter pressed - return final string
            break;
        } else if (ch == 27) { 
            // ESC pressed - cancel edit
            result.clear();
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!result.empty() && cursorPos > 0) {
                result.erase(cursorPos - 1, 1);  // Remove character at cursorPos - 1
                cursorPos--;

                // Redraw the string with shift effect
                mvwprintw(win, startY, startX, "%s ", result.c_str());  // Clear extra char
                wmove(win, startY, startX + cursorPos);
            }
        } else if (ch == KEY_LEFT) {
            if (cursorPos > 0) {
                cursorPos--;
                wmove(win, startY, startX + cursorPos);
            }
        } else if (ch == KEY_RIGHT) {
            if (cursorPos < (int)result.size()) {
                cursorPos++;
                wmove(win, startY, startX + cursorPos);
            }
        } else if (ch >= 32 && ch < 127) {
            // Insert character at cursor position instead of overwriting
            if ((int)result.size() < maxLen) {
                result.insert(cursorPos, 1, static_cast<char>(ch));
                cursorPos++;

                // Redraw entire string to reflect the inserted char
                mvwprintw(win, startY, startX, "%s ", result.c_str());  // Extra space to clear last char
                wmove(win, startY, startX + cursorPos);
            }
        }

        wrefresh(win);
    }

    curs_set(0);  // Hide cursor again
    return result;
}

// Load notifications from NOTIFICATION_FILE
void loadNotifications() {
    std::vector<Notification> notifs;
    std::ifstream inFile(NOTIFICATION_FILE);

    std::string line;
    while (std::getline(inFile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string part;

        Notification n;
        // Format: <epoch_timestamp>;<triggered_flag>;<message>
        if (std::getline(ss, part, ';')) {
            n.scheduledTime = std::stoll(part);
        }
        if (std::getline(ss, part, ';')) {
            n.triggered = (part == "1");
        }
        if (std::getline(ss, part)) {
            n.message = part;
        }
        notifs.push_back(n);
    }
    inFile.close();
    notifications = notifs;
}

// Save notifications to NOTIFICATION_FILE
void saveNotifications() {
    std::ofstream outFile(NOTIFICATION_FILE, std::ios::trunc);

    for (auto &n : notifications) {
        outFile << n.scheduledTime << ";"
                << (n.triggered ? "1" : "0") << ";"
                << n.message << "\n";
    }
    outFile.close();
}

// Load tasks from TODO_FILE
std::vector<Task> loadTasksFromFile() {
    std::vector<Task> result;
    std::ifstream inFile(TODO_FILE);
    if (!inFile.is_open()) {
        return result; // empty if no file
    }

    std::string line;
    while (std::getline(inFile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string part;

        Task t;
        // We assume the format is:
        // dates[0];dates[1];completed;task;category;notification.scheduledTime
        // all separated by semicolons, on one line per task.

        if (std::getline(ss, part, ';')) {
            t.dates[0] = std::stoll(part);
        }
        if (std::getline(ss, part, ';')) {
            t.dates[1] = std::stoll(part);
        }
        if (std::getline(ss, part, ';')) {
            t.completed = (part == "1");
        }
        if (std::getline(ss, part, ';')) {
            t.task = part;
        }
        if (std::getline(ss, part, ';')) {
            t.category = part;
        }
        if (std::getline(ss, part, ';')) {
            t.notification.scheduledTime = std::stoll(part);
        }
        // We'll look up the matching Notification in the global notifications vector
        // if it exists:
        for (size_t i = 0; i < notifications.size(); i++) {
            if (t.notification.scheduledTime == notifications[i].scheduledTime) {
                t.notification = notifications[i];
                break;
            }
        }
        result.push_back(t);
    }
    inFile.close();
    return result;
}

// Save tasks to TODO_FILE (one line per task)
void saveTasks() {
    std::ofstream outFile(TODO_FILE, std::ios::trunc);
    if (!outFile.is_open()) {
        return;
    }
    for (auto &t : allTasks) {
        outFile << t.dates[0] << ";"
                << t.dates[1] << ";"
                << (t.completed ? "1" : "0") << ";"
                << t.task << ";"
                << t.category << ";"
                << t.notification.scheduledTime << "\n";
    }
    outFile.close();
}

// Helper to draw text in a wrapped manner inside a curses window.
static int drawWrappedText(WINDOW* win, int startY, int startX, int width, const std::string& text) {
    if (text.empty()) {
        mvwprintw(win, startY, startX, "%s", "");
        return 1;
    }

    int lineCount = 0;
    int len = (int)text.size();
    int pos = 0;

    while (pos < len) {
        int end = pos + width;
        if (end > len) end = len;

        // Try not to break words
        if (end < len) {
            int tmp = end;
            while (tmp > pos && !std::isspace(static_cast<unsigned char>(text[tmp]))) {
                tmp--;
            }
            if (tmp == pos) {
                // can't break earlier, just break at 'end'
                end = pos + width;
            } else {
                end = tmp;
            }
        }

        int substrLen = end - pos;
        if (substrLen > width) {
            substrLen = width;
        }
        std::string buffer = text.substr(pos, substrLen);
        mvwprintw(win, startY + lineCount, startX, "%s", buffer.c_str());

        // move to the next segment
        pos = end;
        while (pos < len && std::isspace(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
        lineCount++;
    }

    return (lineCount > 0) ? lineCount : 1;
}

// Safely format date/time info for a Task in two columns:
//   - notification time
//   - creation/completion time
// Return a 2-element vector of strings: [reminder_time, relevant_date]
std::vector<std::string> formatDate(const Task &task) {
    std::vector<std::string> tuple(2);

    // If the user has scheduled a reminder in the future
    // (or it might be in the past), let's show that time:
    long long st = task.notification.scheduledTime;

    // We'll pick `task.dates[0]` if not completed, else `task.dates[1]`.
    long long date = (task.completed ? task.dates[1] : task.dates[0]);

    std::time_t tReminder = (std::time_t)st;
    std::time_t tDate     = (std::time_t)date;

    char bufReminder[64];
    char bufDate[64];

    // If st == 0, might mean "no reminder set"
    if (st == 0) {
        snprintf(bufReminder, sizeof(bufReminder), "NoRem");
    } else {
        std::strftime(bufReminder, sizeof(bufReminder), "%Y-%m-%d %H:%M", std::localtime(&tReminder));
    }

    // Format creation/completion date
    std::strftime(bufDate, sizeof(bufDate), "%Y-%m-%d %H:%M", std::localtime(&tDate));

    tuple[0] = bufReminder;
    tuple[1] = bufDate;
    return tuple;
}

// Filter out tasks into currentTasks or completedTasks
void filterTasks(const std::vector<Task>& raw) {
    currentTasks.clear();
    completedTasks.clear();

    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i].completed) {
            completedTasks.push_back(raw[i]);
        } else {
            currentTasks.push_back(raw[i]);
        }
    }
}

// Draw the list portion of the UI
static void drawListUI() {
    // Determine column names based on view mode
    const char* colnames = (viewMode == 0) ? " Current Tasks " : " Completed Tasks ";
    const char* colcat   = " Category ";
    const char* coldates = (viewMode == 0) ? " Added on " : " Completed on ";
    const char* reminder = " Reminder ";

    werase(listWin);
    box(listWin, 0, 0);

    // top line inside the box
    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, "%s", colnames);

    int dateColumnPos     = getmaxx(listWin) - 18;
    int reminderColPos    = getmaxx(listWin) - 56;
    int categoryColumnPos = getmaxx(listWin) - 36;

    mvwprintw(listWin, 0, reminderColPos, "%s", reminder);
    mvwprintw(listWin, 0, categoryColumnPos, "%s", colcat);
    mvwprintw(listWin, 0, dateColumnPos, "%s", coldates);

    // We will refer to either currentTasks or completedTasks
    const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;

    // Build list of tasks that match activeFilterCategory
    std::vector<int> filteredIndices;
    filteredIndices.reserve(temp.size());
    for (int i = 0; i < (int)temp.size(); i++) {
        if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
            filteredIndices.push_back(i);
        }
    }
    // Clamp selectedIndex
    if (!filteredIndices.empty()) {
        if (selectedIndex >= (int)filteredIndices.size()) {
            selectedIndex = (int)filteredIndices.size() - 1;
        }
        if (selectedIndex < 0) {
            selectedIndex = 0;
        }
    } else {
        selectedIndex = 0;
    }

    int taskCount = (int)filteredIndices.size();
    int visibleLines = getmaxy(listWin) - 2;
    int scrollOffset = 0;
    if (selectedIndex >= visibleLines) {
        scrollOffset = selectedIndex - (visibleLines - 1);
    }

    int currentY = 1;
    for (int idx = scrollOffset; idx < taskCount; idx++) {
        if (currentY >= getmaxy(listWin) - 1) {
            break;
        }
        int realIndex = filteredIndices[idx];
        if (idx == selectedIndex) {
            wattron(listWin, COLOR_PAIR(2));
        } else {
            wattron(listWin, COLOR_PAIR(1));
        }

        // Show the item number (1-based)
        mvwprintw(listWin, currentY, 2, "%-3d", realIndex + 1);

        // Show the category
        mvwprintw(listWin, currentY, categoryColumnPos, "%-12s", temp[realIndex].category.c_str());

        // Format the date/time strings
        auto datesStr = formatDate(temp[realIndex]);
        std::string reminderDate = datesStr[0];
        std::string mainDate     = datesStr[1];

        mvwprintw(listWin, currentY, dateColumnPos, "%s", mainDate.c_str());
        mvwprintw(listWin, currentY, reminderColPos, "%s", reminderDate.c_str());

        // The task text (wrapped)
        int linesUsed = drawWrappedText(listWin, currentY, 6,
                                        reminderColPos - 7,
                                        temp[realIndex].task);

        if (idx == selectedIndex) {
            wattroff(listWin, COLOR_PAIR(2));
        } else {
            wattroff(listWin, COLOR_PAIR(1));
        }
        currentY += linesUsed;
    }

    wnoutrefresh(stdscr);
    wnoutrefresh(listWin);
}

static void drawUI() {
    // Draw header on stdscr
    wattron(stdscr, COLOR_PAIR(1));
    mvprintw(1, 2, "CLI TODO APP");
    mvprintw(2, 2, "Current Tasks: %zu | Completed Tasks: %zu",
             currentTasks.size(), completedTasks.size());
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=category, r=reminder, #:filter, Tab=switch, q=save+exit");
    mvprintw(5, 2, "Nav: Up/Down, PgUp/PgDn, Home/End, Goto ':<num>'");
    mvprintw(6, 2, "Category Filter: %s", activeFilterCategory.c_str());
    wattroff(stdscr, COLOR_PAIR(1));

    drawListUI();
}

// Overlay to add a new task
static void addTaskOverlay() {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Enter new task:");
    wrefresh(overlayWin);

    Task new_task;
    new_task.task = ncursesGetString(overlayWin, 2, 2, 1024);

    if (!new_task.task.empty()) {
        // Set creation time
        new_task.dates[0] = get_unix_timestamp();
        new_task.dates[1] = 0; // not completed yet
        new_task.completed = false;
        // Insert it into the master list
        allTasks.push_back(new_task);
        // Also into current tasks
        currentTasks.push_back(new_task);
        // The newly added task is at index = currentTasks.size()-1
        int idx = (int)currentTasks.size() - 1;
        // (because addCategoryOverlay changes currentTasks[idx].category)
        allTasks.back().category = currentTasks[idx].category;

    }
    delwin(overlayWin);
}


// Overlay to add a new task
std::string editTaskOverlay(Task task) {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;
    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Edit task:");
    mvwprintw(overlayWin, 2, 2, "%s", task.task.c_str());
    wrefresh(overlayWin);
    std::string edit_task = ncursesGetString(overlayWin, 2, 2, 1024, task.task);
    delwin(overlayWin);
    return edit_task;
}


// Overlay to set/update category for an item
static void addCategoryOverlay(int taskIndex, bool forCompleted) {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    // Grab a reference to the actual Task we want
    Task &theTask = (forCompleted ? completedTasks[taskIndex]
                                  : currentTasks[taskIndex]);

    mvwprintw(overlayWin, 1, 2, "Enter category for %s item #%d:",
              (forCompleted ? "completed" : "current"), taskIndex+1);

    wmove(overlayWin, 2, 2);
    wrefresh(overlayWin);
    // Pre-fill existing category
    waddstr(overlayWin, theTask.category.c_str());
    wrefresh(overlayWin);

    std::string newCat = ncursesGetString(overlayWin, 2, 2, 1024);
    if (!newCat.empty()) {
        theTask.category = newCat;
    }
    delwin(overlayWin);
}

// Overlay listing categories to filter
static void listCategoriesOverlay() {
    // We gather categories from whichever vector we are viewing
    const std::vector<Task> &vec = (viewMode == 0) ? currentTasks : completedTasks;

    std::set<std::string> uniqueCats;
    for (auto &t : vec) {
        if (!t.category.empty()) {
            uniqueCats.insert(t.category);
        }
    }

    // put them in a vector for indexing
    std::vector<std::string> catList;
    catList.push_back("All");
    for (auto &c : uniqueCats) {
        catList.push_back(c);
    }

    int overlayHeight = 5 + (int)catList.size();
    if (overlayHeight > LINES - 2) {
        overlayHeight = LINES - 2;
    }
    int overlayWidth = 40;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    mvwprintw(overlayWin, 1, 2, "Select a category to filter:");
    wrefresh(overlayWin);

    int catSelected = 0;
    keypad(overlayWin, true);

    while (true) {
        for (int i = 0; i < (int)catList.size(); i++) {
            if (i + 3 >= overlayHeight - 1) {
                break;
            }
            if (i == catSelected) {
                wattron(overlayWin, COLOR_PAIR(2));
            } else {
                wattron(overlayWin, COLOR_PAIR(3));
            }
            mvwprintw(overlayWin, i + 3, 2, "%s  ", catList[i].c_str());
            if (i == catSelected) {
                wattroff(overlayWin, COLOR_PAIR(2));
            } else {
                wattroff(overlayWin, COLOR_PAIR(3));
            }
        }
        wrefresh(overlayWin);

        int ch = wgetch(overlayWin);
        if (ch == KEY_UP) {
            if (catSelected > 0) {
                catSelected--;
            }
        } else if (ch == KEY_DOWN) {
            if (catSelected < (int)catList.size() - 1) {
                catSelected++;
            }
        } else if (ch == 'q' || ch == 27) {
            // q or ESC = cancel
            break;
        } else if (ch == '\n' || ch == '\r') {
            activeFilterCategory = catList[catSelected];
            break;
        }
    }

    delwin(overlayWin);
}

static void completeTask() {
    if (viewMode != 0) return;  // only valid in current-view
    if (currentTasks.empty()) return;

    // Build filteredIndices
    std::vector<int> filteredIndices;
    for (int i = 0; i < (int)currentTasks.size(); i++) {
        if (activeFilterCategory == "All" || currentTasks[i].category == activeFilterCategory) {
            filteredIndices.push_back(i);
        }
    }
    if (filteredIndices.empty()) return;
    if (selectedIndex >= (int)filteredIndices.size()) return;

    int realIndex = filteredIndices[selectedIndex];
    // Mark it completed
    Task t = currentTasks[realIndex];
    t.completed = true;
    t.dates[1] = get_unix_timestamp();

    // Also update in allTasks
    // We can find it by pointer or by some ID, but for simplicity:
    for (int i = 0; i < allTasks.size(); i++) {

        if (allTasks[i].dates[0] == t.dates[0]) {
            allTasks[i].completed = true;
            allTasks[i].dates[1] = t.dates[1];
            break;
        }
    }

    // Move from currentTasks to completedTasks
    completedTasks.push_back(t);
    currentTasks.erase(currentTasks.begin() + realIndex);

    // Adjust index if needed
    if (selectedIndex >= (int)filteredIndices.size()) {
        selectedIndex = (int)filteredIndices.size() - 1;
    }
    if (selectedIndex < 0) selectedIndex = 0;
}



void editTask() {
    const std::vector<Task>& temp = (viewMode == 0) ? currentTasks : completedTasks;
    std::vector<int> filteredIndices;

    for (int i = 0; i < (int)temp.size(); i++) {
        if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
            filteredIndices.push_back(i);
        }
    }

    if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
        int realIndex = filteredIndices[selectedIndex];

        std::string edited_task;
        
        if (viewMode == 0) {
            // Edit current tasks
            Task& updated = currentTasks[realIndex];  // Use reference to modify directly
            edited_task = editTaskOverlay(updated);
            updated.task = edited_task;  // Apply changes

            // Update the corresponding task in allTasks
            for (Task& task : allTasks) {
                if (task.dates[0] == updated.dates[0]) {
                    task.task = edited_task;
                }
            }
        } else {
            // Edit completed tasks
            Task& updated = completedTasks[realIndex];  // Use reference to modify directly
            edited_task = editTaskOverlay(updated);
            updated.task = edited_task;  // Apply changes

            // Update the corresponding task in allTasks
            for (Task& task : allTasks) {
                if (task.dates[0] == updated.dates[0]) {
                    task.task = edited_task;
                }
            }
        }
    }
}


static void deleteTask() {
    // Decide which vector to remove from
    std::vector<Task> &vec = (viewMode == 0) ? currentTasks : completedTasks;
    if (vec.empty()) return;

    std::vector<int> filteredIndices;
    for (int i = 0; i < (int)vec.size(); i++) {
        if (activeFilterCategory == "All" || vec[i].category == activeFilterCategory) {
            filteredIndices.push_back(i);
        }
    }
    if (filteredIndices.empty()) return;
    if (selectedIndex >= (int)filteredIndices.size()) return;

    int realIndex = filteredIndices[selectedIndex];

    // Keep pointer so we can find it in allTasks
    Task delTask = vec[realIndex];

    // Remove from the "master" allTasks as well
    // (One simple approach: compare addresses or compare a unique field like creation time + task text)
    for (int i = 0; i < (int)allTasks.size(); i++) {
        if (allTasks[i].dates[0] == delTask.dates[0]) {
            allTasks.erase(allTasks.begin() + i);
            break;
        }
    }

    vec.erase(vec.begin() + realIndex);

    if (selectedIndex >= (int)filteredIndices.size()) {
        selectedIndex = (int)filteredIndices.size() - 1;
    }
    if (selectedIndex < 0) selectedIndex = 0; 
}

static void gotoItem(int itemNum) {
    // itemNum is 1-based, let's make it 0-based
    itemNum -= 1;

    const std::vector<Task> &vec = (viewMode == 0) ? currentTasks : completedTasks;
    if (itemNum < 0 || itemNum >= (int)vec.size()) return;

    std::vector<int> filteredIndices;
    for (int i = 0; i < (int)vec.size(); i++) {
        if (activeFilterCategory == "All" || vec[i].category == activeFilterCategory) {
            filteredIndices.push_back(i);
        }
    }
    for (int fi = 0; fi < (int)filteredIndices.size(); fi++) {
        if (filteredIndices[fi] == itemNum) {
            selectedIndex = fi;
            return;
        }
    }
}

// Overlay to set an initial reminder time
Notification setReminderOverlay() {
    int overlayHeight = 8;
    int overlayWidth = 60;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    mvwprintw(overlayWin, 1, 2, "Set reminder quantity (integer):");
    wmove(overlayWin, 2, 2);
    wrefresh(overlayWin);

    std::string qtyStr = ncursesGetString(overlayWin, 2, 2, 32);
    long long quantity = 0;
    try {
        quantity = std::stoll(qtyStr);
    } catch (...) {
        quantity = 0;
    }

    mvwprintw(overlayWin, 3, 2, "Choose unit: (s)econds, (m)inutes, (h)ours, (d)ays");
    wrefresh(overlayWin);
    char unitCh = wgetch(overlayWin);
    if (unitCh >= 'A' && unitCh <= 'Z') {
        unitCh = unitCh - 'A' + 'a';
    }

    long long offsetSeconds = convertToSeconds(quantity, unitCh);
    time_t now = std::time(nullptr);

    long long scheduledTime = (long long)now + offsetSeconds;

    Notification newNotif;
    newNotif.scheduledTime = (offsetSeconds == 0 ? 0 : scheduledTime); // if user gave 0 or blank
    newNotif.triggered = false;
    newNotif.message = "Task reminder";

    delwin(overlayWin);
    return newNotif;
}

int main() {
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
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_BLACK, COLOR_WHITE);
    init_pair(3, COLOR_BLUE, COLOR_BLACK);

    int listStartY = 8;
    int listStartX = 2;
    int listHeight = LINES - listStartY - 2;
    int listWidth  = COLS - 4;

    listWin = newwin(listHeight, listWidth, listStartY, listStartX);
    keypad(listWin, true);

    // Load notifications first
    loadNotifications();
    allTasks = loadTasksFromFile();
    filterTasks(allTasks);

    selectedIndex = 0;
    drawUI();
    doupdate();

    while (true) {
        int ch = wgetch(stdscr);
        bool needRedraw = false;
        bool updated = false;
        loadNotifications();

        switch (ch) {
            case 'q':
                // Save all tasks + notifications
                saveTasks();
                loadNotifications();
                saveNotifications();
                delwin(listWin);
                endwin();
                return 0;

            case KEY_UP:
                if (selectedIndex > 0) {
                    selectedIndex--;
                    needRedraw = true;
                }
                break;

            case KEY_DOWN: {
                const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;
                // build filtered indices
                std::vector<int> filteredIndices;
                for (int i = 0; i < (int)temp.size(); i++) {
                    if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
                        filteredIndices.push_back(i);
                    }
                }
                if (selectedIndex < (int)filteredIndices.size() - 1) {
                    selectedIndex++;
                    needRedraw = true;
                }
            } break;

            case KEY_HOME:
                if (selectedIndex != 0) {
                    selectedIndex = 0;
                    needRedraw = true;
                }
                break;

            case KEY_END: {
                const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;
                std::vector<int> filteredIndices;
                for (int i = 0; i < (int)temp.size(); i++) {
                    if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
                        filteredIndices.push_back(i);
                    }
                }
                if (!filteredIndices.empty()) {
                    selectedIndex = (int)filteredIndices.size() - 1;
                    needRedraw = true;
                }
            } break;

            case KEY_PPAGE:
                if (selectedIndex > 10) {
                    selectedIndex -= 10;
                } else {
                    selectedIndex = 0;
                }
                needRedraw = true;
                break;

            case KEY_NPAGE: {
                const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;
                std::vector<int> filteredIndices;
                for (int i = 0; i < (int)temp.size(); i++) {
                    if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
                        filteredIndices.push_back(i);
                    }
                }
                int limit = (int)filteredIndices.size();
                if (selectedIndex + 10 < limit) {
                    selectedIndex += 10;
                } else {
                    if (limit > 0) selectedIndex = limit - 1;
                    else selectedIndex = 0;
                }
                needRedraw = true;
            } break;

            case 'r': {
                
                const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;
                std::vector<int> filteredIndices;
                for (int i = 0; i < (int)temp.size(); i++) {
                    if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
                        filteredIndices.push_back(i);
                    }
                }

                Notification new_reminder = setReminderOverlay();
                if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
                    int realIndex = filteredIndices[selectedIndex];
                    // Also update allTasks so we don't lose the new category
                    if (viewMode == 0) {
               
                        Task &updated = currentTasks[realIndex];
                        for (auto &A : allTasks) {
                            if (A.dates[0] == updated.dates[0] &&
                                A.task == updated.task) {
                                new_reminder.message = A.task;
                                A.notification = new_reminder;
                            }
                        }

                    } else {
                        // completed
                        Task &updated = completedTasks[realIndex];
                        for (auto &A : allTasks) {
                            if (A.dates[0] == updated.dates[0] &&
                                A.task == updated.task) {
                                new_reminder.message = A.task;
                                A.notification = new_reminder;
                            }
                        }
                    }
                }
                notifications.push_back(new_reminder);
                saveNotifications();
            } 
                needRedraw = true;
                break;

            case 'n':
                addTaskOverlay();
                needRedraw = true;
                break;

            case 'c':
                completeTask();
                needRedraw = true;
                break;

            case 'd':
                deleteTask();
                needRedraw = true;
                break;

            case 's': {
                const std::vector<Task> &temp = (viewMode == 0) ? currentTasks : completedTasks;
                std::vector<int> filteredIndices;
                for (int i = 0; i < (int)temp.size(); i++) {
                    if (activeFilterCategory == "All" || temp[i].category == activeFilterCategory) {
                        filteredIndices.push_back(i);
                    }
                }
                if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
                    int realIndex = filteredIndices[selectedIndex];
                    // show overlay
                    addCategoryOverlay(realIndex, (viewMode == 1));
                    // Also update allTasks so we don't lose the new category
                    if (viewMode == 0) {
                        // current
                        // find pointer
                        Task &updated = currentTasks[realIndex];
                        for (auto &A : allTasks) {
                            if (A.dates[0] == updated.dates[0] &&
                                A.task == updated.task) {
                                A.category = updated.category;
                            }
                        }
                    } else {
                        // completed
                        Task &updated = completedTasks[realIndex];
                        for (auto &A : allTasks) {
                            if (A.dates[0] == updated.dates[0] &&
                                A.task == updated.task) {
                                A.category = updated.category;
                            }
                        }
                    }
                    needRedraw = true;
                }
            } break;

            case '#':
                listCategoriesOverlay();
                needRedraw = true;
                break;

            case 'e':
                editTask();
                needRedraw = true;
                break;

            case ':': {
                mvprintw(LINES - 1, 0, "Goto item (blank=cancel): ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();
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
                        int itemNum = std::stoi(lineInput);
                        gotoItem(itemNum);
                        needRedraw = true;
                    } catch (...) {
                        // ignore invalid input
                    }
                }
                mvprintw(LINES - 1, 0, "                                              ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();
            } break;

            case '\t':
                viewMode = 1 - viewMode;
                selectedIndex = 0;
                needRedraw = true;
                break;

            default:
                break;
        }

        if (needRedraw) {
            drawUI();
            doupdate();
        }
    }

    delwin(listWin);
    endwin();
    return 0;
}

