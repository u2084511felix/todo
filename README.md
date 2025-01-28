# C++ CLI TODO App

A simple CLI type todo app. Written in C++ for linux systems. This tool provides a simple CLI interface to help manage daily tasks in the terminal. Future features may include:
- Integrated cross-platform push notifications (including mobile).
- More sophisticated categorization features.
- Timers and chron jobs associated with tasks.
- AI LLM-agent integration for automatic task execution, or task creation.

### Dependencies and Target System
This program is designed for **Linux systems**, specifically tested on Ubuntu 22.04, where dependencies are included by default. On older systems, you may need to install:
- `g++` (GNU C++ compiler)
- `libncurses-dev` (ncurses development library)

Refer to the [Dependencies Installation](#dependencies-installation) section below for instructions if required.

---

## Run / Install

1. Clone the repository or copy the source code to your local machine:
   ```bash
   git clone https://github.com/u2084511felix/todo
   cd todo 
   ```

2. 
   a) Create a symbolic link to make the binary executable from anywhere (e.g., `/usr/local/bin`):
   ```bash
   sudo ln -s $(pwd)/todo /usr/local/bin/todo
   ```

   or

   b) Compile the program yourself:
   ```bash
   g++ -o todo todo.cpp -lncurses
   ```
   Then do step 2(a).

3. Run the program from anywhere:
   ```bash
   todo
   ```

---

## Keyboard Controls

| Key             | Function                                        |
|-----------------|------------------------------------------------|
| **UP/DOWN**     | Navigate through the file list                 |
| **ENTER**       | Toggle the `assume-unchanged` state of a file  |
| **TAB**         | Switch between "Current" and "Completed"       |
| **Q**           | Quit the application                           |
| **C**           | Complete a task                               |
| **D**           | Delete a task                                 |
| **N**           | Add a new task                                |
| **S**           | Set a category                                |
| **#**           | Filter categories                             |
| **PgUp/PgDn**   | Navigate by page                              |
| **Home/End**    | Jump to the beginning or end of the list       |
| **':<num>'**    | Go to a specific line number                  |
| **Q**           | Save and exit                                 |

---

## Notes

- Make sure to use absolute path in the symlink. 
- Feel free to fork this repo and make pull requests. Just make sure to do git assume-unchanged on your current and completed files.

---

## Dependencies Installation
If running on older systems (e.g., Ubuntu versions prior to 22.04), install the required dependencies:

```bash
sudo apt update
sudo apt install g++ libncurses-dev
```
