#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <set>
#include <cctype>    // needed for isspace
#include <algorithm> // for potential string trimming, find_if, etc.
#include <ncurses.h>

#include <filesystem>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>    // for std::this_thread::sleep_for
#include <ctime>     // for std::time_t

namespace fs = std::filesystem;

const std::string resourceDir = std::string(std::getenv("HOME")) + "/todo/";
const std::string currentFile = resourceDir + "current.txt";
const std::string completedFile = resourceDir + "completed.txt";

static std::vector<std::string> currentTasks;
static std::vector<std::string> currentDates;
static std::vector<std::string> currentCategories;

// New fields: reminder time (epoch) + overdue interval (hours).
// We store them as strings in memory, but we'll parse them as time_t or int.
static std::vector<std::string> currentReminderEpoch;       // e.g., "0" means no reminder
static std::vector<std::string> currentReminderIntervalHrs; // e.g., "0" means no overdue frequency

static std::vector<std::string> completedTasks;
static std::vector<std::string> completedDates;
static std::vector<std::string> completedCategories;
static std::vector<std::string> completedReminderEpoch;
static std::vector<std::string> completedReminderIntervalHrs;

static int selectedIndex = 0;
static int viewMode = 0;  // 0 = current, 1 = completed
static std::string activeFilterCategory = "All"; // "All" = no filter
static WINDOW* listWin = nullptr;

// Forward declarations
static void drawUI();
static void addTaskOverlay();
static void addCategoryOverlay(int taskIndex, bool forCompleted);
static void listCategoriesOverlay();
static void completeTask();
static void deleteTask();
static void gotoItem(int itemNum);
static void setReminderOverlay(int taskIndex, bool forCompleted);
static void setOverdueFrequencyOverlay(int taskIndex, bool forCompleted);

static std::string ncursesGetString(WINDOW* win, int startY, int startX, int maxLen = 1024);

// ---------------------------------------------------------------
// EXTENDED SPLIT: We'll parse up to 5 fields, separated by '|':
// task|date|category|reminderEpoch|reminderIntervalHrs
// If any field is missing, default to empty string or "0" for numeric fields.
// ---------------------------------------------------------------
static void extendedSplitTaskLine(
    const std::string& line,
    std::string& task,
    std::string& date,
    std::string& cat,
    std::string& reminderEpoch,
    std::string& reminderIntervalHrs)
{
    // We'll do a simple approach: split by '|'.
    // Then fill missing fields if not present.
    std::vector<std::string> parts;
    {
        size_t start = 0;
        while (true) {
            size_t pos = line.find('|', start);
            if (pos == std::string::npos) {
                // last chunk
                parts.push_back(line.substr(start));
                break;
            } else {
                parts.push_back(line.substr(start, pos - start));
                start = pos + 1;
            }
        }
    }

    // default everything
    task.clear();
    date.clear();
    cat.clear();
    reminderEpoch = "0";
    reminderIntervalHrs = "0";

    if (!parts.empty()) task = parts[0];
    if (parts.size() > 1) date = parts[1];
    if (parts.size() > 2) cat  = parts[2];
    if (parts.size() > 3) reminderEpoch = parts[3];
    if (parts.size() > 4) reminderIntervalHrs = parts[4];
}

// ---------------------------------------------------------------
// EXTENDED JOIN: We'll build a line with up to 5 fields separated by '|'
// ---------------------------------------------------------------
static std::string extendedJoinTaskLine(
    const std::string &task,
    const std::string &date,
    const std::string &cat,
    const std::string &reminderEpoch,
    const std::string &reminderIntervalHrs)
{
    // Format: task|date|cat|reminderEpoch|reminderIntervalHrs
    // (We do minimal escaping, assuming user doesn't use '|').
    std::ostringstream oss;
    oss << task << "|"
        << date << "|"
        << cat << "|"
        << reminderEpoch << "|"
        << reminderIntervalHrs;
    return oss.str();
}

// ---------------------------------------------------------------
static void loadTasks(const std::string& file,
                      std::vector<std::string>& tasks,
                      std::vector<std::string>& dates,
                      std::vector<std::string>& categories,
                      std::vector<std::string>& reminderEpochs,
                      std::vector<std::string>& reminderIntervals)
{
    std::ifstream fin(file);
    if (!fin) return;

    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        std::string t, d, c, re, ri;
        extendedSplitTaskLine(line, t, d, c, re, ri);
        if (!t.empty()) {
            tasks.push_back(t);
            dates.push_back(d);
            categories.push_back(c);
            reminderEpochs.push_back(re);
            reminderIntervals.push_back(ri);
        }
    }
}

static void saveTasks(const std::string& file,
                      const std::vector<std::string>& tasks,
                      const std::vector<std::string>& dates,
                      const std::vector<std::string>& categories,
                      const std::vector<std::string>& reminderEpochs,
                      const std::vector<std::string>& reminderIntervals)
{
    std::ofstream fout(file);
    if (!fout) return;

    for (size_t i = 0; i < tasks.size(); ++i) {
        std::string line = extendedJoinTaskLine(
            tasks[i],
            dates[i],
            categories[i],
            reminderEpochs[i],
            reminderIntervals[i]
        );
        fout << line << "\n";
    }
}

// Helper to wrap text in curses.
static int drawWrappedText(WINDOW* win, int startY, int startX, int width, const std::string& text) {
    if (text.empty()) {
        mvwprintw(win, startY, startX, "%s", "");
        return 1;
    }

    int lineCount = 0;
    int len = static_cast<int>(text.size());
    int pos = 0;

    while (pos < len) {
        int end = pos + width;
        if (end > len) end = len;

        // Try not to break words.
        if (end < len) {
            int tmp = end;
            while (tmp > pos && !std::isspace((unsigned char)text[tmp])) {
                tmp--;
            }
            if (tmp == pos) {
                // no space found, force the split
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
        while (pos < len && std::isspace((unsigned char)text[pos])) {
            pos++;
        }
        lineCount++;
    }

    return (lineCount > 0) ? lineCount : 1;
}

static void drawUI() {
    // Header
    wattron(stdscr, COLOR_PAIR(1));
    mvprintw(1, 2, "CLI TODO APP - with Reminders");
    mvprintw(2, 2, "Current Tasks: %zu | Completed Tasks: %zu",
             currentTasks.size(), completedTasks.size());
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=set category, R=set reminder, O=overdue freq");
    mvprintw(5, 2, "   #:filter categories, Tab=switch file, q=save+exit");
    mvprintw(6, 2, "   Goto ':<num>', Overdue freq min=1h max=168h");
    mvprintw(7, 2, "Category Filter: %s", activeFilterCategory.c_str());
    wattroff(stdscr, COLOR_PAIR(1));

    werase(listWin);
    box(listWin, 0, 0);

    const char* colnames = (viewMode == 0) ? " Current Tasks " : " Completed Tasks ";
    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, "%s", colnames);

    int dateColumnPos = getmaxx(listWin) - 14;     // date
    int categoryColumnPos = getmaxx(listWin) - 26; // category
    mvwprintw(listWin, 0, categoryColumnPos, " Category ");
    mvwprintw(listWin, 0, dateColumnPos, (viewMode == 0) ? " Added on " : " Completed ");

    // pick the vectors
    const std::vector<std::string>* tasks;
    const std::vector<std::string>* dates;
    const std::vector<std::string>* categories;
    // We won't display reminder fields in the main UI (to keep it simpler),
    // but you could show them if you like.

    if (viewMode == 0) {
        tasks      = &currentTasks;
        dates      = &currentDates;
        categories = &currentCategories;
    } else {
        tasks      = &completedTasks;
        dates      = &completedDates;
        categories = &completedCategories;
    }

    // build filtered list
    std::vector<int> filteredIndices;
    filteredIndices.reserve(tasks->size());
    for (int i = 0; i < (int)tasks->size(); i++) {
        if (activeFilterCategory == "All" ||
            (*categories)[i] == activeFilterCategory)
        {
            filteredIndices.push_back(i);
        }
    }

    // clamp selectedIndex
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

        mvwprintw(listWin, currentY, 2, "%-3d", realIndex + 1);
        mvwprintw(listWin, currentY, categoryColumnPos, "%-12s",
                  (*categories)[realIndex].c_str());
        mvwprintw(listWin, currentY, dateColumnPos, "%s",
                  (*dates)[realIndex].c_str());

        // task text
        int linesUsed = drawWrappedText(listWin, currentY, 6,
                                        categoryColumnPos - 7,
                                        (*tasks)[realIndex]);

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

    std::string newTask = ncursesGetString(overlayWin, 2, 2, 1024);
    if (!newTask.empty()) {
        currentTasks.push_back(newTask);

        // date/time
        auto now = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now);
        std::tm localTm = *std::localtime(&now_t);
        std::ostringstream oss;
        oss << std::put_time(&localTm, "%Y-%m-%d");
        currentDates.push_back(oss.str());

        // default
        currentCategories.push_back("");
        currentReminderEpoch.push_back("0");
        currentReminderIntervalHrs.push_back("0");

        // optionally let user set category immediately
        addCategoryOverlay((int)currentTasks.size() - 1, false);
        // Let user set a reminder right away (optional)
        setReminderOverlay((int)currentTasks.size() - 1, false);
        setOverdueFrequencyOverlay((int)currentTasks.size() - 1, false);
    }
    delwin(overlayWin);
}

static void addCategoryOverlay(int taskIndex, bool forCompleted) {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    std::string existingCat;
    if (!forCompleted) {
        existingCat = currentCategories[taskIndex];
        mvwprintw(overlayWin, 1, 2, "Enter category for current item #%d:", taskIndex + 1);
    } else {
        existingCat = completedCategories[taskIndex];
        mvwprintw(overlayWin, 1, 2, "Enter category for completed item #%d:", taskIndex + 1);
    }
    wmove(overlayWin, 2, 2);
    wrefresh(overlayWin);

    waddstr(overlayWin, existingCat.c_str());
    std::string newCat = ncursesGetString(overlayWin, 2, 2, 1024);

    if (!forCompleted) {
        if (!newCat.empty() || existingCat.empty()) {
            currentCategories[taskIndex] = newCat;
        }
    } else {
        if (!newCat.empty() || existingCat.empty()) {
            completedCategories[taskIndex] = newCat;
        }
    }
    delwin(overlayWin);
}

// ------------------------------------------------------------------
// Overlay to set the initial reminder time (in hours or days).
// We'll store as "reminderEpoch" = (current epoch + offset).
// ------------------------------------------------------------------
static void setReminderOverlay(int taskIndex, bool forCompleted) {
    int overlayHeight = 8;
    int overlayWidth = 60;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    mvwprintw(overlayWin, 1, 2, "Set reminder time (e.g. '5h' or '2d').");
    mvwprintw(overlayWin, 2, 2, "Min = 1h, Max = 7d. Empty = no reminder.");
    mvwprintw(overlayWin, 4, 2, "Input: ");
    wrefresh(overlayWin);

    std::string input = ncursesGetString(overlayWin, 4, 10, 32);
    if (input.empty()) {
        // user canceled or empty => no reminder
        if (!forCompleted) {
            currentReminderEpoch[taskIndex] = "0";
        } else {
            completedReminderEpoch[taskIndex] = "0";
        }
        delwin(overlayWin);
        return;
    }

    // parse input
    // Expect something like '5h', '2d', etc.
    // We'll handle only those two formats for simplicity.
    long hours = 0;
    bool valid = false;

    try {
        if (input.size() > 1) {
            char suffix = input.back();
            std::string numberPart = input.substr(0, input.size() - 1);
            long val = std::stol(numberPart);

            if (suffix == 'h' || suffix == 'H') {
                hours = val;
                valid = true;
            } else if (suffix == 'd' || suffix == 'D') {
                hours = val * 24;
                valid = true;
            }
        }
    } catch (...) {
        // parse error
    }

    if (!valid || hours < 1) {
        // minimal check, user asked for something invalid or < 1h
        // we treat it as "no reminder"
        hours = 0;
    }
    if (hours > 24 * 7) {
        // clamp to 7 days = 168h
        hours = 168;
    }

    // If hours == 0 => no reminder
    if (hours == 0) {
        if (!forCompleted) currentReminderEpoch[taskIndex] = "0";
        else completedReminderEpoch[taskIndex] = "0";
    } else {
        // set the epoch
        auto now = std::chrono::system_clock::now();
        auto nowSec = std::chrono::system_clock::to_time_t(now);
        long reminderSec = nowSec + hours * 3600L;
        std::ostringstream oss;
        oss << reminderSec;

        if (!forCompleted) {
            currentReminderEpoch[taskIndex] = oss.str();
        } else {
            completedReminderEpoch[taskIndex] = oss.str();
        }
    }

    delwin(overlayWin);
}

// ------------------------------------------------------------------
// Overlay to set the overdue reminder interval, in hours or days.
// We'll store as an integer (in hours). min=1, max=168. 
// ------------------------------------------------------------------
static void setOverdueFrequencyOverlay(int taskIndex, bool forCompleted) {
    int overlayHeight = 8;
    int overlayWidth = 60;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    mvwprintw(overlayWin, 1, 2, "Set OVERDUE reminder frequency (e.g. '1h' or '2d').");
    mvwprintw(overlayWin, 2, 2, "Min=1h, Max=7d (168h). Empty=no repeated alerts.");
    mvwprintw(overlayWin, 4, 2, "Input: ");
    wrefresh(overlayWin);

    std::string input = ncursesGetString(overlayWin, 4, 10, 32);
    if (input.empty()) {
        // user canceled => set to "0"
        if (!forCompleted) {
            currentReminderIntervalHrs[taskIndex] = "0";
        } else {
            completedReminderIntervalHrs[taskIndex] = "0";
        }
        delwin(overlayWin);
        return;
    }

    // parse input
    long freqHours = 0;
    bool valid = false;

    try {
        if (input.size() > 1) {
            char suffix = input.back();
            std::string numberPart = input.substr(0, input.size() - 1);
            long val = std::stol(numberPart);
            if (suffix == 'h' || suffix == 'H') {
                freqHours = val;
                valid = true;
            } else if (suffix == 'd' || suffix == 'D') {
                freqHours = val * 24;
                valid = true;
            }
        }
    } catch (...) {}

    if (!valid || freqHours < 1) {
        freqHours = 0; // no repeated alerts
    }
    if (freqHours > 168) {
        freqHours = 168;
    }

    // store
    {
        std::ostringstream oss;
        oss << freqHours;
        if (!forCompleted) {
            currentReminderIntervalHrs[taskIndex] = oss.str();
        } else {
            completedReminderIntervalHrs[taskIndex] = oss.str();
        }
    }

    delwin(overlayWin);
}

// ------------------------------------------------------------------
static void completeTask() {
    if (viewMode != 0) return; // only from current -> completed
    if (currentTasks.empty()) return;

    // build filtered indices
    std::vector<int> filtered;
    for (int i = 0; i < (int)currentTasks.size(); i++) {
        if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
            filtered.push_back(i);
        }
    }
    if (filtered.empty()) return;
    if (selectedIndex >= (int)filtered.size()) return;

    int realIndex = filtered[selectedIndex];

    // move to completed
    completedTasks.push_back(currentTasks[realIndex]);
    completedDates.push_back(currentDates[realIndex]);
    completedCategories.push_back(currentCategories[realIndex]);
    completedReminderEpoch.push_back(currentReminderEpoch[realIndex]);
    completedReminderIntervalHrs.push_back(currentReminderIntervalHrs[realIndex]);

    // remove from current
    currentTasks.erase(currentTasks.begin() + realIndex);
    currentDates.erase(currentDates.begin() + realIndex);
    currentCategories.erase(currentCategories.begin() + realIndex);
    currentReminderEpoch.erase(currentReminderEpoch.begin() + realIndex);
    currentReminderIntervalHrs.erase(currentReminderIntervalHrs.begin() + realIndex);

    if (selectedIndex >= (int)filtered.size()) {
        selectedIndex = (int)filtered.size() - 1;
    }
    if (selectedIndex < 0) selectedIndex = 0;
}

static void deleteTask() {
    if (viewMode == 0) {
        if (currentTasks.empty()) return;
        std::vector<int> filtered;
        for (int i = 0; i < (int)currentTasks.size(); i++) {
            if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                filtered.push_back(i);
            }
        }
        if (filtered.empty()) return;
        if (selectedIndex >= (int)filtered.size()) return;

        int realIndex = filtered[selectedIndex];
        currentTasks.erase(currentTasks.begin() + realIndex);
        currentDates.erase(currentDates.begin() + realIndex);
        currentCategories.erase(currentCategories.begin() + realIndex);
        currentReminderEpoch.erase(currentReminderEpoch.begin() + realIndex);
        currentReminderIntervalHrs.erase(currentReminderIntervalHrs.begin() + realIndex);

        if (selectedIndex >= (int)filtered.size()) {
            selectedIndex = (int)filtered.size() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    } else {
        if (completedTasks.empty()) return;
        std::vector<int> filtered;
        for (int i = 0; i < (int)completedTasks.size(); i++) {
            if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                filtered.push_back(i);
            }
        }
        if (filtered.empty()) return;
        if (selectedIndex >= (int)filtered.size()) return;

        int realIndex = filtered[selectedIndex];
        completedTasks.erase(completedTasks.begin() + realIndex);
        completedDates.erase(completedDates.begin() + realIndex);
        completedCategories.erase(completedCategories.begin() + realIndex);
        completedReminderEpoch.erase(completedReminderEpoch.begin() + realIndex);
        completedReminderIntervalHrs.erase(completedReminderIntervalHrs.begin() + realIndex);

        if (selectedIndex >= (int)filtered.size()) {
            selectedIndex = (int)filtered.size() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    }
}

static void gotoItem(int itemNum) {
    // itemNum is 1-based
    itemNum -= 1;
    if (viewMode == 0) {
        if (itemNum < 0 || itemNum >= (int)currentTasks.size()) return;
        std::vector<int> filtered;
        for (int i = 0; i < (int)currentTasks.size(); i++) {
            if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                filtered.push_back(i);
            }
        }
        for (int fi = 0; fi < (int)filtered.size(); fi++) {
            if (filtered[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
    } else {
        if (itemNum < 0 || itemNum >= (int)completedTasks.size()) return;
        std::vector<int> filtered;
        for (int i = 0; i < (int)completedTasks.size(); i++) {
            if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                filtered.push_back(i);
            }
        }
        for (int fi = 0; fi < (int)filtered.size(); fi++) {
            if (filtered[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
    }
}

// ------------------------------------------------------------------
// List categories overlay
// ------------------------------------------------------------------
static void listCategoriesOverlay() {
    const std::vector<std::string>* cats =
        (viewMode == 0) ? &currentCategories : &completedCategories;

    std::set<std::string> uniqueCats;
    for (const auto &c : *cats) {
        if (!c.empty()) {
            uniqueCats.insert(c);
        }
    }

    std::vector<std::string> catList;
    catList.push_back("All");
    for (const auto &c : uniqueCats) {
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
            if (i+3 >= overlayHeight-1) break;
            if (i == catSelected) {
                wattron(overlayWin, COLOR_PAIR(2));
            } else {
                wattron(overlayWin, COLOR_PAIR(3));
            }
            mvwprintw(overlayWin, i+3, 2, "%s  ", catList[i].c_str());
            if (i == catSelected) {
                wattroff(overlayWin, COLOR_PAIR(2));
            } else {
                wattroff(overlayWin, COLOR_PAIR(3));
            }
        }
        wrefresh(overlayWin);

        int ch = wgetch(overlayWin);
        if (ch == KEY_UP) {
            if (catSelected > 0) catSelected--;
        } else if (ch == KEY_DOWN) {
            if (catSelected < (int)catList.size()-1) catSelected++;
        } else if (ch == 'q' || ch == 27) {
            break; // cancel
        } else if (ch == '\n') {
            activeFilterCategory = catList[catSelected];
            break;
        }
    }

    delwin(overlayWin);
}

// ------------------------------------------------------------------
// Check and ensure resource directory/files exist
// ------------------------------------------------------------------
void ensureResourcesExist() {
    if (!fs::exists(resourceDir)) {
        std::cout << "Creating resource directory: " << resourceDir << std::endl;
        fs::create_directories(resourceDir);
    }
    if (!fs::exists(currentFile)) {
        std::cout << "Creating default resource file: " << currentFile << std::endl;
        std::ofstream file(currentFile);
        file << "";
    }
    if (!fs::exists(completedFile)) {
        std::cout << "Creating default resource file: " << completedFile << std::endl;
        std::ofstream file(completedFile);
        file << "";
    }
}

bool hasWriteAccess(const std::string& path) {
    if (access(path.c_str(), W_OK) == 0) {
        return true;
    } else {
        std::cerr << "Write access denied for path: " << path << std::endl;
        return false;
    }
}

// ------------------------------------------------------------------
// Daemon mode: Periodically check reminders and overdue intervals.
// This is a simple demonstration that repeatedly checks the tasks
// from disk. If a reminder is due, we do some form of "alert".
// ------------------------------------------------------------------

void runDaemonLoop() {
    std::cout << "[Daemon] Starting reminder loop...\n";

    while (true) {
        // 1. Reload tasks from disk each iteration
        std::vector<std::string> tasks, dates, cats, reEpochs, reIntervals;
        loadTasks(currentFile, tasks, dates, cats, reEpochs, reIntervals);

        // 2. Current time
        auto now = std::chrono::system_clock::now();
        std::time_t nowSec = std::chrono::system_clock::to_time_t(now);

        // 3. Check each task’s reminder time
        for (size_t i = 0; i < tasks.size(); i++) {
            long epoch = std::stol(reEpochs[i]); // reminderEpoch
            if (epoch > 0 && nowSec >= epoch) {
                // -- The task's reminder time has arrived or is overdue --

                // (A) Send a desktop notification using notify-send
                //     Construct a command string, then invoke system().
                //     Title = "Reminder", body = Task name, etc.
                std::string title = "Reminder";
                std::string body  = "Task \"" + tasks[i] + "\" is due!";
                std::string cmd   = "notify-send \"" + title + "\" \"" + body + "\"";
                system(cmd.c_str());

                // (B) Log to stdout, or syslog if desired
                std::cout << "[Reminder] " << body << std::endl;

                // (C) If there's a repeat interval for overdue tasks
                long freqH = std::stol(reIntervals[i]);
                if (freqH > 0) {
                    // Bump the next reminder by freqH hours
                    epoch += freqH * 3600;
                    reEpochs[i] = std::to_string(epoch);
                } else {
                    // No repeated alerts => set to 0 to avoid repeated spamming
                    reEpochs[i] = "0";
                }
            }
        }

        // 4. Save updated tasks back to disk
        saveTasks(currentFile, tasks, dates, cats, reEpochs, reIntervals);

        // 5. Sleep before next check
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

int main(int argc, char** argv) {
    // If run with --daemon, run the background reminder loop
    if (argc > 1 && std::string(argv[1]) == "--daemon") {
        ensureResourcesExist();
        if (!hasWriteAccess(resourceDir)) {
            std::cerr << "Cannot write to resource directory: " << resourceDir << std::endl;
            return 1;
        }
        runDaemonLoop();
        return 0;
    }

    // Otherwise, normal CLI usage
    ensureResourcesExist();

    if (!hasWriteAccess(resourceDir)) {
        std::cerr << "Cannot write to resource directory: " << resourceDir << std::endl;
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
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_BLACK, COLOR_WHITE);
    init_pair(3, COLOR_BLUE, COLOR_BLACK);

    int listStartY = 8;
    int listStartX = 2;
    int listHeight = LINES - listStartY - 2;
    int listWidth = COLS - 4;
    listWin = newwin(listHeight, listWidth, listStartY, listStartX);
    keypad(listWin, true);

    // load tasks
    loadTasks(currentFile, 
              currentTasks, currentDates, currentCategories,
              currentReminderEpoch, currentReminderIntervalHrs);
    loadTasks(completedFile,
              completedTasks, completedDates, completedCategories,
              completedReminderEpoch, completedReminderIntervalHrs);

    selectedIndex = 0;
    drawUI();
    doupdate();

    while (true) {
        int ch = wgetch(stdscr);
        bool needRedraw = false;

        switch (ch) {
            case 'q':
                // Save before exit
                saveTasks(currentFile, 
                          currentTasks, currentDates, currentCategories,
                          currentReminderEpoch, currentReminderIntervalHrs);
                saveTasks(completedFile,
                          completedTasks, completedDates, completedCategories,
                          completedReminderEpoch, completedReminderIntervalHrs);
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
                // build filtered
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (selectedIndex < (int)filteredIndices.size()-1) {
                    selectedIndex++;
                    needRedraw = true;
                }
            } break;

            case KEY_HOME:
                selectedIndex = 0;
                needRedraw = true;
                break;

            case KEY_END: {
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty()) {
                    selectedIndex = (int)filteredIndices.size()-1;
                }
                needRedraw = true;
            } break;

            case KEY_PPAGE:
                if (selectedIndex > 10) selectedIndex -= 10;
                else selectedIndex = 0;
                needRedraw = true;
                break;

            case KEY_NPAGE: {
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                int limit = (int)filteredIndices.size();
                if (selectedIndex + 10 < limit) {
                    selectedIndex += 10;
                } else {
                    if (limit > 0) selectedIndex = limit - 1;
                }
                needRedraw = true;
            } break;

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
                // set category
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
                    int realIndex = filteredIndices[selectedIndex];
                    if (viewMode == 0) {
                        addCategoryOverlay(realIndex, false);
                    } else {
                        addCategoryOverlay(realIndex, true);
                    }
                    needRedraw = true;
                }
            } break;

            case 'R': {
                // Set reminder time
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
                    int realIndex = filteredIndices[selectedIndex];
                    if (viewMode == 0) {
                        setReminderOverlay(realIndex, false);
                    } else {
                        setReminderOverlay(realIndex, true);
                    }
                    needRedraw = true;
                }
            } break;

            case 'O': {
                // set overdue frequency
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < (int)currentTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            currentCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < (int)completedTasks.size(); i++) {
                        if (activeFilterCategory == "All" ||
                            completedCategories[i] == activeFilterCategory)
                        {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty() && selectedIndex < (int)filteredIndices.size()) {
                    int realIndex = filteredIndices[selectedIndex];
                    if (viewMode == 0) {
                        setOverdueFrequencyOverlay(realIndex, false);
                    } else {
                        setOverdueFrequencyOverlay(realIndex, true);
                    }
                    needRedraw = true;
                }
            } break;

            case '#':
                listCategoriesOverlay();
                needRedraw = true;
                break;

            case ':': {
                mvprintw(LINES - 1, 0, "Goto item (Enter to cancel): ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();
                echo();
                curs_set(1);

                char buffer[16] = {0};
                wgetnstr(stdscr, buffer, 15);
                noecho();
                curs_set(0);

                std::string lineInput(buffer);
                if (!lineInput.empty()) {
                    try {
                        int itemNum = std::stoi(lineInput);
                        gotoItem(itemNum);
                        needRedraw = true;
                    } catch (...) {}
                }

                mvprintw(LINES - 1, 0, "                                              ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();
            } break;

            case '\t':
                viewMode = !viewMode;
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

// ---------------------------------------------------------------
// Implementation of ncursesGetString(...)
// ---------------------------------------------------------------
static std::string ncursesGetString(WINDOW* win, int startY, int startX, int maxLen) {
    std::string result;
    int ch = 0;
    wmove(win, startY, startX);
    wrefresh(win);

    curs_set(1);
    while (true) {
        ch = wgetch(win);
        if (ch == '\n' || ch == '\r') {
            break;
        } else if (ch == 27) { // ESC = cancel
            result.clear();
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!result.empty()) {
                result.pop_back();
                int y, x;
                getyx(win, y, x);
                if (x > startX) {
                    mvwaddch(win, y, x-1, ' ');
                    wmove(win, y, x-1);
                }
            }
        } else if (ch == KEY_LEFT || ch == KEY_RIGHT ||
                   ch == KEY_UP   || ch == KEY_DOWN) {
            // ignore arrow keys
            continue;
        } else if (ch >= 32 && ch < 127) {
            if ((int)result.size() < maxLen) {
                result.push_back((char)ch);
                waddch(win, ch);
            }
        }
        wrefresh(win);
    }
    curs_set(0);
    return result;
}

