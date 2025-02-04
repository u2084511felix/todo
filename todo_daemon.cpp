#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>    // for system()
#include <ctime>      // for time()


// TODO: inotify + epoll. Waits for file modification.

//
//
// Path to the file storing scheduled notifications.
// Make sure your service has permission to read/write this file.

static const std::string NOTIFICATION_FILE = "/var/lib/todo/notifications.db";
// A simple struct to hold notification data
struct Notification {
    long long scheduledTime; // epoch timestamp when to trigger
    bool triggered;          // has the notification been sent?
    std::string message;     // the notification text
};

// Helper function to load all notifications from file
std::vector<Notification> loadNotifications() {
    std::vector<Notification> notifs;
    std::ifstream inFile(NOTIFICATION_FILE);
    if (!inFile.is_open()) {
        // If file can't be opened, return empty vector
        return notifs;
    }

    std::string line;
    while (std::getline(inFile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string part;

        Notification n;
        // Format: epoch_timestamp;triggered_flag;message
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
    return notifs;
}

// Helper function to save all notifications back to file
void saveNotifications(const std::vector<Notification>& notifs) {
    std::ofstream outFile(NOTIFICATION_FILE, std::ios::trunc); // overwrite
    if (!outFile.is_open()) {
        std::cerr << "ERROR: Unable to write to " << NOTIFICATION_FILE << std::endl;
        return;
    }
    for (auto &n : notifs) {
        outFile << n.scheduledTime << ";"
                << (n.triggered ? "1" : "0") << ";"
                << n.message << "\n";
    }
    outFile.close();
}

int main() {
    std::cout << "my_daemon started. Monitoring scheduled notifications...\n";

    // Create the file if it doesn't exist (so we can open it without errors)
    {
        std::ofstream createFile(NOTIFICATION_FILE, std::ios::app);
        // Just ensure file exists. Do nothing else.
    }

    while (true) {
        // Load notifications
        auto notifs = loadNotifications();

        // Current epoch time
        auto now = static_cast<long long>(std::time(nullptr));
        bool updated = false;

        // Check each notification
        for (auto &n : notifs) {
            // If not triggered and time has come
            if (!n.triggered && (n.scheduledTime <= now && n.scheduledTime > (now - 2))) {
                // Send the notification (using notify-send)
                std::string command = "notify-send '~@~TODO!~@~' '" + n.message + "'";
                system(command.c_str());
                n.triggered = true;
                updated = true;
            }
        }

        // If we triggered any new notifications, save them
        if (updated) {
            saveNotifications(notifs);
        }

        // Sleep some seconds before checking again
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
