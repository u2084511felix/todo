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

#include <cstdlib> // for system calls like getenv

#include <sys/stat.h> // for mkdir

#include <unistd.h>   // for access()

namespace fs = std::filesystem;

// Constants for the resource directory and file names
const std::string resourceDir = std::string(std::getenv("HOME")) + "/todo/";
const std::string currentFile = resourceDir + "current.txt";
const std::string completedFile = resourceDir + "completed.txt";




// We'll store the current and completed tasks in vectors:
static std::vector<std::string> currentTasks;
static std::vector<std::string> currentDates;
static std::vector<std::string> currentCategories;

static std::vector<std::string> completedTasks;
static std::vector<std::string> completedDates;
static std::vector<std::string> completedCategories;

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

// Helper to read user input in a more C++-style way.
static std::string ncursesGetString(WINDOW* win, int startY, int startX, int maxLen = 1024) {
    std::string result;
    int ch = 0;
    wmove(win, startY, startX);
    wrefresh(win);

    // Enable cursor for input
    curs_set(1);

    while (true) {
        ch = wgetch(win);

        if (ch == '\n' || ch == '\r') {
            // Enter pressed
            break;
        } else if (ch == 27) {
            // q pressed, treat as cancel -> return empty string
            result.clear();
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!result.empty()) {
                result.pop_back();
                // Move cursor left, overwrite char with space, move cursor left again
                int y, x;
                getyx(win, y, x);
                if (x > startX) {
                    mvwaddch(win, y, x-1, ' ');
                    wmove(win, y, x-1);
                }
            }
        } else if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == KEY_UP || ch == KEY_DOWN) {
            // ignore arrow keys here
            continue;
        } else if (ch >= 32 && ch < 127) {
            // Basic printable ASCII range
            if ((int)result.size() < maxLen) {
                result.push_back(static_cast<char>(ch));
                waddch(win, ch);
            }
        }
        wrefresh(win);
    }

    curs_set(0); // hide cursor again
    return result;
}

// A small helper to split a string by delimiter, up to 3 parts (task|date|cat).
static void splitTaskLine(const std::string& line,
                          std::string& task,
                          std::string& date,
                          std::string& cat) {
    // Find first separator
    std::size_t firstSep = line.find('|');
    if (firstSep == std::string::npos) {
        // no valid data
        task.clear();
        date.clear();
        cat.clear();
        return;
    }

    std::size_t secondSep = line.find('|', firstSep + 1);

    task = line.substr(0, firstSep);
    if (secondSep == std::string::npos) {
        // Then we only have task|date
        date = line.substr(firstSep + 1);
        cat = "";
    } else {
        date = line.substr(firstSep + 1, secondSep - (firstSep + 1));
        cat = line.substr(secondSep + 1);
    }
}

// Load tasks from file. Format: task|date|cat
static void loadTasks(const std::string& file,
                      std::vector<std::string>& tasks,
                      std::vector<std::string>& dates,
                      std::vector<std::string>& categories) {
    std::ifstream fin(file);
    if (!fin) {
        return; // file didn't open, no tasks loaded
    }

    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty()) {
            continue;
        }
        std::string task, date, cat;
        splitTaskLine(line, task, date, cat);
        if (!task.empty()) {
            tasks.push_back(task);
            dates.push_back(date);
            categories.push_back(cat);
        }
    }
}

// Save tasks to file. Format: task|date|category
static void saveTasks(const std::string& file,
                      const std::vector<std::string>& tasks,
                      const std::vector<std::string>& dates,
                      const std::vector<std::string>& categories) {
    std::ofstream fout(file);
    if (!fout) {
        return;
    }

    for (std::size_t i = 0; i < tasks.size(); i++) {
        fout << tasks[i] << "|" << dates[i] << "|" << categories[i] << "\n";
    }
}

// Helper to draw text in a wrapped manner inside a curses window.
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
            while (tmp > pos && !std::isspace(static_cast<unsigned char>(text[tmp]))) {
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
        while (pos < len && std::isspace(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
        lineCount++;
    }

    return (lineCount > 0) ? lineCount : 1;
}

static void drawUI() {
    // Draw header on stdscr
    wattron(stdscr, COLOR_PAIR(1));
    mvprintw(1, 2, "CLI TODO APP");
    mvprintw(2, 2, "Current Tasks: %zu | Completed Tasks: %zu",
             currentTasks.size(), completedTasks.size());
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=set category, #:filter categories, Tab=switch file, q=save+exit");
    mvprintw(5, 2, "Nav: Up/Down, PgUp/PgDn, Home/End, Goto ':<num>'");
    mvprintw(6, 2, "Category Filter: %s", activeFilterCategory.c_str());
    wattroff(stdscr, COLOR_PAIR(1));

    // Determine column names based on view mode
    const char* colnames = (viewMode == 0) ? " Current Tasks " : " Completed Tasks ";
    const char* colcat   = " Category ";
    const char* coldates = (viewMode == 0) ? " Added on " : " Completed on ";

    werase(listWin);
    box(listWin, 0, 0);
    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, "%s", colnames);

    int dateColumnPos = getmaxx(listWin) - 14;     // date is displayed here
    int categoryColumnPos = getmaxx(listWin) - 26; // spacing for category
    mvwprintw(listWin, 0, categoryColumnPos, "%s", colcat);
    mvwprintw(listWin, 0, dateColumnPos, "%s", coldates);

    // pick the vectors
    const std::vector<std::string>* tasks;
    const std::vector<std::string>* dates;
    const std::vector<std::string>* categories;

    if (viewMode == 0) {
        tasks = &currentTasks;
        dates = &currentDates;
        categories = &currentCategories;
    } else {
        tasks = &completedTasks;
        dates = &completedDates;
        categories = &completedCategories;
    }

    // build filtered list
    std::vector<int> filteredIndices;
    filteredIndices.reserve(tasks->size());
    for (int i = 0; i < static_cast<int>(tasks->size()); i++) {
        if (activeFilterCategory == "All" ||
            (*categories)[i] == activeFilterCategory)
        {
            filteredIndices.push_back(i);
        }
    }

    // clamp selectedIndex
    if (!filteredIndices.empty()) {
        if (selectedIndex >= static_cast<int>(filteredIndices.size())) {
            selectedIndex = static_cast<int>(filteredIndices.size()) - 1;
        }
        if (selectedIndex < 0) {
            selectedIndex = 0;
        }
    } else {
        selectedIndex = 0;
    }

    int taskCount = static_cast<int>(filteredIndices.size());
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

        // category
        mvwprintw(listWin, currentY, categoryColumnPos, "%-12s",
                  (*categories)[realIndex].c_str());

        // date
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

// Overlay to add a new task.
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

    // read user input with modern approach
    std::string newTask = ncursesGetString(overlayWin, 2, 2, 1024);

    if (!newTask.empty()) {
        // create item
        currentTasks.push_back(newTask);

        // get date/time in modern c++ way
        auto now = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now);
        std::tm localTm = *std::localtime(&now_t);
        std::ostringstream oss;
        oss << std::put_time(&localTm, "%Y-%m-%d");
        currentDates.push_back(oss.str());

        // default no category
        currentCategories.push_back("");

        // overlay to set category immediately
        addCategoryOverlay(static_cast<int>(currentTasks.size()) - 1, false);
    }

    delwin(overlayWin);
}

// Overlay to set or update category for an item.
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

    // let's prefill. We'll just print it out, then let the user edit if desired.
    waddstr(overlayWin, existingCat.c_str());

    // now let's do input in place. We'll do a simple approach: read fresh.
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

// Overlay listing categories.
static void listCategoriesOverlay() {
    // gather categories from whichever mode
    const std::vector<std::string>* cats = (viewMode == 0) ? &currentCategories : &completedCategories;
    std::set<std::string> uniqueCats;
    for (const auto &c : *cats) {
        if (!c.empty()) {
            uniqueCats.insert(c);
        }
    }

    // put them in a vector for indexing
    std::vector<std::string> catList;
    catList.push_back("All");
    for (const auto &c : uniqueCats) {
        catList.push_back(c);
    }

    int overlayHeight = 5 + static_cast<int>(catList.size());
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
        // draw the category list
        for (int i = 0; i < static_cast<int>(catList.size()); i++) {
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
            if (catSelected < static_cast<int>(catList.size()) - 1) {
                catSelected++;
            }
        } else if (ch == 'q' || ch == 27) {
            // q = cancel
            break;
        } else if (ch == '\n' || ch == '\r') {
            activeFilterCategory = catList[catSelected];
            break;
        }
    }

    delwin(overlayWin);
}

static void completeTask() {
    // only in viewMode 0
    if (viewMode == 0 && !currentTasks.empty()) {
        // build filtered indices
        std::vector<int> filteredIndices;
        filteredIndices.reserve(currentTasks.size());
        for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
            if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= static_cast<int>(filteredIndices.size())) return;

        int realIndex = filteredIndices[selectedIndex];

        // move item to completed
        completedTasks.push_back(currentTasks[realIndex]);
        completedDates.push_back(currentDates[realIndex]);
        completedCategories.push_back(currentCategories[realIndex]);

        // remove from current
        currentTasks.erase(currentTasks.begin() + realIndex);
        currentDates.erase(currentDates.begin() + realIndex);
        currentCategories.erase(currentCategories.begin() + realIndex);

        if (selectedIndex >= static_cast<int>(filteredIndices.size())) {
            selectedIndex = static_cast<int>(filteredIndices.size()) - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    }
}

static void deleteTask() {
    if (viewMode == 0) {
        if (currentTasks.empty()) return;
        std::vector<int> filteredIndices;
        filteredIndices.reserve(currentTasks.size());
        for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
            if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= static_cast<int>(filteredIndices.size())) return;

        int realIndex = filteredIndices[selectedIndex];
        currentTasks.erase(currentTasks.begin() + realIndex);
        currentDates.erase(currentDates.begin() + realIndex);
        currentCategories.erase(currentCategories.begin() + realIndex);

        if (selectedIndex >= static_cast<int>(filteredIndices.size())) {
            selectedIndex = static_cast<int>(filteredIndices.size()) - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    } else {
        if (completedTasks.empty()) return;
        std::vector<int> filteredIndices;
        filteredIndices.reserve(completedTasks.size());
        for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
            if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= static_cast<int>(filteredIndices.size())) return;

        int realIndex = filteredIndices[selectedIndex];
        completedTasks.erase(completedTasks.begin() + realIndex);
        completedDates.erase(completedDates.begin() + realIndex);
        completedCategories.erase(completedCategories.begin() + realIndex);

        if (selectedIndex >= static_cast<int>(filteredIndices.size())) {
            selectedIndex = static_cast<int>(filteredIndices.size()) - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    }
}

static void gotoItem(int itemNum) {
    // itemNum is 1-based, let's make it 0-based
    itemNum -= 1;
    if (viewMode == 0) {
        if (itemNum < 0 || itemNum >= static_cast<int>(currentTasks.size())) return;
        std::vector<int> filteredIndices;
        for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
            if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                filteredIndices.push_back(i);
            }
        }
        for (int fi = 0; fi < static_cast<int>(filteredIndices.size()); fi++) {
            if (filteredIndices[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
    } else {
        if (itemNum < 0 || itemNum >= static_cast<int>(completedTasks.size())) return;
        std::vector<int> filteredIndices;
        for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
            if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                filteredIndices.push_back(i);
            }
        }
        for (int fi = 0; fi < static_cast<int>(filteredIndices.size()); fi++) {
            if (filteredIndices[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
    }
}



// Function to check and ensure the resource directory and files exist
void ensureResourcesExist() {
    // Check if the resource directory exists
    if (!fs::exists(resourceDir)) {
        std::cout << "Creating resource directory: " << resourceDir << std::endl;
        fs::create_directories(resourceDir); // Create the directory
    }

    // Check and create the resource files if they don't exist
    if (!fs::exists(currentFile)) {
        std::cout << "Creating default resource file: " << currentFile << std::endl;
        std::ofstream file(currentFile);
        file << ""; // Initialize as empty
    }

    if (!fs::exists(completedFile)) {
        std::cout << "Creating default resource file: " << completedFile << std::endl;
        std::ofstream file(completedFile);
        file << ""; // Initialize as empty
    }
}

// Function to check if the program has write access to the directory
bool hasWriteAccess(const std::string& path) {
    if (access(path.c_str(), W_OK) == 0) {
        return true;
    } else {
        std::cerr << "Write access denied for path: " << path << std::endl;
        return false;
    }
}


int main() {

    // Ensure resource directory and files exist
    ensureResourcesExist();

    // Verify write access to the directory
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

    // Load tasks using absolute paths
    loadTasks(currentFile, currentTasks, currentDates, currentCategories);
    loadTasks(completedFile, completedTasks, completedDates, completedCategories);

    selectedIndex = 0;

    drawUI();
    doupdate();

    while (true) {
        int ch = wgetch(stdscr);
        bool needRedraw = false;

        switch (ch) {
            case 'q':
                // Save tasks using absolute paths
                saveTasks(currentFile, currentTasks, currentDates, currentCategories);
                saveTasks(completedFile, completedTasks, completedDates, completedCategories);
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
                // find how many in filtered list
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
                        if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
                        if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (selectedIndex < static_cast<int>(filteredIndices.size()) - 1) {
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
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
                        if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
                        if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty()) {
                    selectedIndex = static_cast<int>(filteredIndices.size()) - 1;
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
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
                        if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
                        if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                int limit = static_cast<int>(filteredIndices.size());
                if (selectedIndex + 10 < limit) {
                    selectedIndex += 10;
                } else {
                    if (limit > 0) selectedIndex = limit - 1;
                    else selectedIndex = 0;
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
                std::vector<int> filteredIndices;
                if (viewMode == 0) {
                    for (int i = 0; i < static_cast<int>(currentTasks.size()); i++) {
                        if (activeFilterCategory == "All" || currentCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                } else {
                    for (int i = 0; i < static_cast<int>(completedTasks.size()); i++) {
                        if (activeFilterCategory == "All" || completedCategories[i] == activeFilterCategory) {
                            filteredIndices.push_back(i);
                        }
                    }
                }
                if (!filteredIndices.empty() && selectedIndex < static_cast<int>(filteredIndices.size())) {
                    int realIndex = filteredIndices[selectedIndex];
                    if (viewMode == 0) {
                        addCategoryOverlay(realIndex, false);
                    } else {
                        addCategoryOverlay(realIndex, true);
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

                char buffer[16] = {0}; // Initialize buffer directly instead of using memset

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
