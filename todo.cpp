#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <ncurses.h>

using namespace std;


// testing gitstream

// We can keep maximums for demonstration purposes, but now we are mainly relying on vectors.
static const int MAX_TASKS = 1000;
static const int MAX_TASK_LENGTH = 1024;

// We'll store the current and completed tasks in vectors:
vector<string> currentTasks;
vector<string> currentDates;
vector<string> currentCategories;

vector<string> completedTasks;
vector<string> completedDates;
vector<string> completedCategories;

int selectedIndex = 0;
int viewMode = 0;  // 0 = current, 1 = completed

// This category filter determines which items we display.
// "All" means no filter; otherwise, show only tasks with matching category.
string activeFilterCategory = "All";

WINDOW* listWin;

// Forward declarations
void drawUI();
void addTaskOverlay();
void addCategoryOverlay(int taskIndex, bool forCompleted);
void listCategoriesOverlay();

/**
 * Load tasks from a file into the provided vectors.
 * Expects each line in the format:  taskText|date|category
 * (Category can be empty.)
 */
void loadTasks(const string& file,
               vector<string>& tasks,
               vector<string>& dates,
               vector<string>& categories)
{
    ifstream fin(file);
    if (!fin) {
        return;  // If file doesn't exist or can't be opened, just return
    }

    string line;
    while (true) {
        if (!std::getline(fin, line)) {
            break;  // no more lines
        }

        // Split by '|' up to 3 parts: task, date, category
        // We'll do a simple approach using find and substr.
        size_t firstSep = line.find('|');
        if (firstSep == string::npos) {
            continue;
        }
        size_t secondSep = line.find('|', firstSep + 1);

        string task = line.substr(0, firstSep);
        string date;
        string cat;

        if (secondSep == string::npos) {
            // Then we only have task|date
            date = line.substr(firstSep + 1);
            cat = "";
        } else {
            date = line.substr(firstSep + 1, secondSep - (firstSep + 1));
            cat = line.substr(secondSep + 1);
        }

        // Clean trailing newlines
        if (!date.empty() && date.back() == '\n') {
            date.pop_back();
        }
        if (!cat.empty() && cat.back() == '\n') {
            cat.pop_back();
        }

        tasks.push_back(task);
        dates.push_back(date);
        categories.push_back(cat);
    }

    fin.close();
}

/**
 * Save tasks into a file from the provided vectors.
 * Format: task|date|category
 */
void saveTasks(const string& file,
               const vector<string>& tasks,
               const vector<string>& dates,
               const vector<string>& categories)
{
    ofstream fout(file);
    if (!fout) {
        return;
    }

    for (size_t i = 0; i < tasks.size(); i++) {
        fout << tasks[i] << "|" << dates[i] << "|" << categories[i] << "\n";
    }

    fout.close();
}

/**
 * Draw wrapped text in the specified window.
 * Returns the number of lines printed.
 */
int drawWrappedText(WINDOW* win, int startY, int startX, int width, const string& text) {
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

        // Try not to break words
        if (end < len) {
            // Move backwards until we find a space or reach pos
            int tmp = end;
            while (tmp > pos && !isspace(static_cast<unsigned char>(text[tmp]))) {
                tmp--;
            }
            if (tmp == pos) {
                // No space found, force split
                end = pos + width;
            } else {
                end = tmp;
            }
        }

        // Extract the substring
        int substrLen = end - pos;
        if (substrLen > width) {
            substrLen = width;
        }
        string buffer = text.substr(pos, substrLen);

        // Print the line
        mvwprintw(win, startY + lineCount, startX, "%s", buffer.c_str());

        // Move to the next segment
        pos = end;
        // Skip any spaces
        while (pos < len && isspace(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
        lineCount++;
    }

    return (lineCount > 0) ? lineCount : 1;
}

/**
 * Draw the user interface, including headers and the task list.
 * Now we filter by activeFilterCategory (if not "All").
 */
void drawUI() {
    // Draw header on stdscr
    attron(COLOR_PAIR(1));
    mvprintw(1, 2, "CLI TODO APP");
    mvprintw(2, 2, "Current Tasks: %lu | Completed Tasks: %lu",
             currentTasks.size(), completedTasks.size());
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    mvprintw(4, 2, "Keys: c=complete, d=delete, n=add, s=set category, #:filter categories, Tab=switch file, ESC=save+exit");
    mvprintw(5, 2, "Nav: Up/Down, PgUp/PgDn, Home/End, Goto ':<num>'");
    mvprintw(6, 2, "Category Filter: %s", activeFilterCategory.c_str());
    attroff(COLOR_PAIR(1));

    // Determine column names based on view mode
    const char* colnames = (viewMode == 0) ? " Current Tasks " : " Completed Tasks ";
    const char* colcat   = " Category ";
    const char* coldates = (viewMode == 0) ? " Added on " : " Completed on ";

    // Clear and redraw the list window
    werase(listWin);
    box(listWin, 0, 0);
    mvwprintw(listWin, 0, 2, " # ");
    mvwprintw(listWin, 0, 6, "%s", colnames);
    // We'll put Category a bit to the right from the tasks, let's say near the far right
    // Adjust your layout as desired
    int dateColumnPos = getmaxx(listWin) - 14;     // where date is displayed
    int categoryColumnPos = getmaxx(listWin) - 26; // some spacing for category
    mvwprintw(listWin, 0, categoryColumnPos, "%s", colcat);
    mvwprintw(listWin, 0, dateColumnPos, "%s", coldates);

    // Select the appropriate vectors
    const vector<string>* tasks;
    const vector<string>* dates;
    const vector<string>* categories;
    if (viewMode == 0) {
        tasks = &currentTasks;
        dates = &currentDates;
        categories = &currentCategories;
    } else {
        tasks = &completedTasks;
        dates = &completedDates;
        categories = &completedCategories;
    }

    // Build a list of indices that match our category filter
    vector<int> filteredIndices;
    filteredIndices.reserve(tasks->size());
    for (int i = 0; i < (int)tasks->size(); i++) {
        if (activeFilterCategory == "All" ||
            (*categories)[i] == activeFilterCategory)
        {
            filteredIndices.push_back(i);
        }
    }

    // Ensure selectedIndex is in range of filtered items
    if (!filteredIndices.empty()) {
        if (selectedIndex >= (int)filteredIndices.size()) {
            selectedIndex = (int)filteredIndices.size() - 1;
        }
        if (selectedIndex < 0) {
            selectedIndex = 0;
        }
    } else {
        // No items in the filter, set selectedIndex to 0
        selectedIndex = 0;
    }

    int taskCount = (int)filteredIndices.size();

    // Calculate visible lines and scroll offset
    int visibleLines = getmaxy(listWin) - 2;
    int scrollOffset = 0;
    // If selectedIndex is too far down, we scroll so that selected item is visible.
    // A simple approach:
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

        // Print item number
        mvwprintw(listWin, currentY, 2, "%-3d", realIndex + 1);

        // Print category
        mvwprintw(listWin, currentY, categoryColumnPos, "%-12s",
                  (*categories)[realIndex].c_str());

        // Print date (far right)
        mvwprintw(listWin, currentY, dateColumnPos, "%s",
                  (*dates)[realIndex].c_str());

        // Print wrapped task text
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

    // Prepare windows for refreshing
    wnoutrefresh(stdscr);
    wnoutrefresh(listWin);
}

/**
 * Display an overlay window to add a new task.
 * After user enters a task, if not empty, we also prompt for category.
 */
void addTaskOverlay() {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);
    mvwprintw(overlayWin, 1, 2, "Enter new task:");
    wrefresh(overlayWin);

    char input[MAX_TASK_LENGTH] = {0};
    echo();
    curs_set(1); // Show cursor for input
    mvwgetnstr(overlayWin, 2, 2, input, MAX_TASK_LENGTH - 1);
    noecho();
    curs_set(0); // Hide cursor after input

    // If the user actually typed something:
    if (strlen(input) > 0 && currentTasks.size() < MAX_TASKS) {
        currentTasks.push_back(string(input));
        time_t now = time(nullptr);
        char dateStr[20];
        strftime(dateStr, 20, "%Y-%m-%d", localtime(&now));
        currentDates.push_back(dateStr);
        // Default no category
        currentCategories.push_back("");

        // Prompt for a category right away
        addCategoryOverlay((int)currentTasks.size() - 1, false);
    }

    delwin(overlayWin);
}

/**
 * Overlay to set (or update) the category of a specific item.
 */
void addCategoryOverlay(int taskIndex, bool forCompleted) {
    int overlayHeight = 7;
    int overlayWidth = COLS - 20;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    // If forCompleted = false => current tasks, else completed tasks
    string existingCat;
    if (!forCompleted) {
        existingCat = currentCategories[taskIndex];
        mvwprintw(overlayWin, 1, 2, "Enter category for current item #%d:", taskIndex + 1);
    } else {
        existingCat = completedCategories[taskIndex];
        mvwprintw(overlayWin, 1, 2, "Enter category for completed item #%d:", taskIndex + 1);
    }
    wrefresh(overlayWin);

    char input[MAX_TASK_LENGTH] = {0};
    // Pre-fill with existing category
    strncpy(input, existingCat.c_str(), MAX_TASK_LENGTH - 1);

    echo();
    curs_set(1);
    mvwgetnstr(overlayWin, 2, 2, input, MAX_TASK_LENGTH - 1);
    noecho();
    curs_set(0);

    // Store the new category (or empty if user cleared it)
    if (!forCompleted) {
        currentCategories[taskIndex] = string(input);
    } else {
        completedCategories[taskIndex] = string(input);
    }

    delwin(overlayWin);
}

/**
 * Overlay listing unique categories, plus "All" at the top.
 * User picks one, which sets activeFilterCategory.
 */
void listCategoriesOverlay() {
    // Gather categories from whichever view we are in
    const vector<string>* cats;
    if (viewMode == 0) {
        cats = &currentCategories;
    } else {
        cats = &completedCategories;
    }

    // Build a set of unique categories
    set<string> uniqueCats;
    for (auto &c : *cats) {
        if (!c.empty()) {
            uniqueCats.insert(c);
        }
    }

    // We'll store them in a vector so we can index them
    vector<string> catList;
    // Put "All" on top
    catList.push_back("All");
    for (auto &c : uniqueCats) {
        catList.push_back(c);
    }

    int overlayHeight = 5 + (int)catList.size();
    if (overlayHeight > LINES - 2) overlayHeight = LINES - 2; // clamp
    int overlayWidth = 40;
    int overlayY = (LINES - overlayHeight) / 2;
    int overlayX = (COLS - overlayWidth) / 2;

    WINDOW* overlayWin = newwin(overlayHeight, overlayWidth, overlayY, overlayX);
    wbkgd(overlayWin, COLOR_PAIR(3));
    box(overlayWin, 0, 0);

    mvwprintw(overlayWin, 1, 2, "Select a category to filter:");
    wrefresh(overlayWin);

    // We'll do a simple selection with up/down, enter to confirm, ESC to cancel
    int catSelected = 0; // index in catList
    keypad(overlayWin, TRUE);

    while (true) {
        // Draw the category list
        for (int i = 0; i < (int)catList.size(); i++) {
            if (i+3 >= overlayHeight - 1) break; // no more space
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
        } else if (ch == 27) {
            // ESC = cancel, do not change the filter
            break;
        } else if (ch == '\n') {
            // pick the category
            activeFilterCategory = catList[catSelected];
            break;
        }
    }

    delwin(overlayWin);
}

/**
 * Mark the selected current task as completed.
 * We use the *filtered index* to locate real index.
 */
void completeTask() {
    // In viewMode 0, we can complete an item
    if (viewMode == 0 && !currentTasks.empty()) {
        // Build filtered index
        vector<int> filteredIndices;
        for (int i = 0; i < (int)currentTasks.size(); i++) {
            if (activeFilterCategory == "All" ||
                currentCategories[i] == activeFilterCategory)
            {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= (int)filteredIndices.size()) return;

        int realIndex = filteredIndices[selectedIndex];

        if (completedTasks.size() < MAX_TASKS) {
            // Move item to completed
            completedTasks.push_back(currentTasks[realIndex]);
            completedDates.push_back(currentDates[realIndex]);
            completedCategories.push_back(currentCategories[realIndex]);
        }
        // Erase from current
        currentTasks.erase(currentTasks.begin() + realIndex);
        currentDates.erase(currentDates.begin() + realIndex);
        currentCategories.erase(currentCategories.begin() + realIndex);

        // Adjust selectedIndex
        if (selectedIndex >= (int)filteredIndices.size()) {
            selectedIndex = (int)filteredIndices.size() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    }
}

/**
 * Delete the selected task from the current or completed list.
 */
void deleteTask() {
    if (viewMode == 0) {
        // Build filtered index
        vector<int> filteredIndices;
        for (int i = 0; i < (int)currentTasks.size(); i++) {
            if (activeFilterCategory == "All" ||
                currentCategories[i] == activeFilterCategory)
            {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= (int)filteredIndices.size()) return;
        int realIndex = filteredIndices[selectedIndex];

        currentTasks.erase(currentTasks.begin() + realIndex);
        currentDates.erase(currentDates.begin() + realIndex);
        currentCategories.erase(currentCategories.begin() + realIndex);

        if (selectedIndex >= (int)filteredIndices.size()) {
            selectedIndex = (int)filteredIndices.size() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    } else {
        vector<int> filteredIndices;
        for (int i = 0; i < (int)completedTasks.size(); i++) {
            if (activeFilterCategory == "All" ||
                completedCategories[i] == activeFilterCategory)
            {
                filteredIndices.push_back(i);
            }
        }
        if (filteredIndices.empty()) return;
        if (selectedIndex >= (int)filteredIndices.size()) return;
        int realIndex = filteredIndices[selectedIndex];

        completedTasks.erase(completedTasks.begin() + realIndex);
        completedDates.erase(completedDates.begin() + realIndex);
        completedCategories.erase(completedCategories.begin() + realIndex);

        if (selectedIndex >= (int)filteredIndices.size()) {
            selectedIndex = (int)filteredIndices.size() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
    }
}

/**
 * Jump to a specific item number in the *unfiltered* list. 
 * For convenience, let's adapt it to go to that item in the *full* list,
 * then see if it passes the filter. If it doesn't, the user won't see it.
 */
void gotoItem(int itemNum) {
    // In practice, you might want to align this with the filtered list approach.
    // But for simplicity, we'll just set selectedIndex so it lines up with
    // that item in unfiltered space, ignoring filter. Then the next draw
    // might shift you around if that item is not in the filter.
    // 
    // A robust approach: build the filtered list, find which filtered index
    // corresponds to itemNum - 1 in real index. 
    // We'll do the robust approach:
    itemNum--; // make 0-based
    if (viewMode == 0) {
        if (itemNum < 0 || itemNum >= (int)currentTasks.size()) return;

        // Build filteredIndices
        vector<int> filteredIndices;
        for (int i = 0; i < (int)currentTasks.size(); i++) {
            if (activeFilterCategory == "All" ||
                currentCategories[i] == activeFilterCategory)
            {
                filteredIndices.push_back(i);
            }
        }
        // Find which index in filteredIndices corresponds to itemNum
        for (int fi = 0; fi < (int)filteredIndices.size(); fi++) {
            if (filteredIndices[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
        // If not found, do nothing
    } else {
        if (itemNum < 0 || itemNum >= (int)completedTasks.size()) return;

        vector<int> filteredIndices;
        for (int i = 0; i < (int)completedTasks.size(); i++) {
            if (activeFilterCategory == "All" ||
                completedCategories[i] == activeFilterCategory)
            {
                filteredIndices.push_back(i);
            }
        }
        // Find which index in filteredIndices corresponds to itemNum
        for (int fi = 0; fi < (int)filteredIndices.size(); fi++) {
            if (filteredIndices[fi] == itemNum) {
                selectedIndex = fi;
                return;
            }
        }
    }
}

int main() {
    // Initialize ncurses
    initscr();
    cbreak();              // Pass keystrokes immediately
    noecho();             // Do not echo input characters
    keypad(stdscr, TRUE);  // Enable special keys
    curs_set(0);          // Hide cursor

    // Initialize color pairs
    if (has_colors() == FALSE) {
        endwin();
        cout << "Your terminal does not support color" << endl;
        exit(1);
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);  // Regular text
    init_pair(2, COLOR_BLACK, COLOR_WHITE);  // Highlighted text
    init_pair(3, COLOR_BLUE, COLOR_BLACK);   // Overlay background

    // Create the list window
    int listStartY = 8, listStartX = 2;
    int listHeight = LINES - listStartY - 2;
    int listWidth  = COLS - 4;
    listWin = newwin(listHeight, listWidth, listStartY, listStartX);
    keypad(listWin, TRUE); // Enable special keys for list window

    // Load tasks from files
    loadTasks("current.txt",   currentTasks,   currentDates,   currentCategories);
    loadTasks("completed.txt", completedTasks, completedDates, completedCategories);

    // Ensure selectedIndex is within bounds
    selectedIndex = 0;

    // Initial draw
    drawUI();
    doupdate(); // Batch refresh to minimize flicker

    int ch;
    while (true) {
        ch = wgetch(stdscr); // Get input from stdscr

        bool needRedraw = false;

        switch (ch) {
            case 27: // ESC key -> exit
                // Save tasks before exiting
                saveTasks("current.txt",
                          currentTasks,
                          currentDates,
                          currentCategories);
                saveTasks("completed.txt",
                          completedTasks,
                          completedDates,
                          completedCategories);
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
                // We need to see how many items are in the filtered list
                vector<int> filteredIndices;
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
                if (selectedIndex < (int)filteredIndices.size() - 1) {
                    selectedIndex++;
                    needRedraw = true;
                }
                break;
            }

            case KEY_HOME:
                if (selectedIndex != 0) {
                    selectedIndex = 0;
                    needRedraw = true;
                }
                break;

            case KEY_END: {
                // Figure out the size of the filtered list
                vector<int> filteredIndices;
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
                    selectedIndex = (int)filteredIndices.size() - 1;
                    needRedraw = true;
                }
                break;
            }

            case KEY_PPAGE: {
                if (selectedIndex > 10) {
                    selectedIndex -= 10;
                } else {
                    selectedIndex = 0;
                }
                needRedraw = true;
                break;
            }

            case KEY_NPAGE: {
                // Check how many filtered items
                vector<int> filteredIndices;
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
                    else selectedIndex = 0;
                }
                needRedraw = true;
                break;
            }

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
                // Set category for selected item
                // We need the real index from filtered list
                vector<int> filteredIndices;
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
                if (!filteredIndices.empty() &&
                    selectedIndex < (int)filteredIndices.size())
                {
                    int realIndex = filteredIndices[selectedIndex];
                    if (viewMode == 0) {
                        addCategoryOverlay(realIndex, false);
                    } else {
                        addCategoryOverlay(realIndex, true);
                    }
                    needRedraw = true;
                }
                break;
            }

            case '#':
                // Open category filter overlay
                listCategoriesOverlay();
                needRedraw = true;
                break;

            case ':': {
                // Prompt for item number to go to
                mvprintw(LINES - 1, 0, "Goto item (Enter to cancel): ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();

                char lineInput[10] = {0};
                int idx = 0;
                int c2;

                curs_set(1); // Show cursor
                echo();
                while ((c2 = wgetch(stdscr)) != '\n') {
                    if (c2 == KEY_BACKSPACE || c2 == 127 || c2 == '\b') {
                        if (idx > 0) {
                            idx--;
                            lineInput[idx] = '\0';
                            mvprintw(LINES - 1, 0, "Goto item (Enter to cancel): %s ", lineInput);
                            clrtoeol();
                        }
                    } else if (isdigit(c2) && idx < 9) {
                        lineInput[idx++] = static_cast<char>(c2);
                        lineInput[idx] = '\0';
                        mvprintw(LINES - 1, 0, "Goto item (Enter to cancel): %s", lineInput);
                        clrtoeol();
                    }
                    wnoutrefresh(stdscr);
                    doupdate();
                }
                noecho();
                curs_set(0); // Hide cursor

                if (lineInput[0] != '\0') {
                    int itemNum = atoi(lineInput);
                    gotoItem(itemNum);
                    needRedraw = true;
                }

                // Clear the prompt line
                mvprintw(LINES - 1, 0, "                                        ");
                clrtoeol();
                wnoutrefresh(stdscr);
                doupdate();
                break;
            }

            case '\t': // Tab key to switch views
                viewMode = !viewMode;
                // Reset selectedIndex to 0 or within range
                selectedIndex = 0;
                needRedraw = true;
                break;

            default:
                break;
        }

        // Redraw UI if needed
        if (needRedraw) {
            drawUI();
            doupdate();
        }
    }

    // Cleanup
    delwin(listWin);
    endwin();
    return 0;
}
