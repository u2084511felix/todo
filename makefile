CC = g++
CFLAGS = -Wall -std=c++17
LDFLAGS = -lncurses -lsqlite3
TARGET = todosql
DAEMON = todo_daemon_sql

all: $(TARGET) $(DAEMON)

$(TARGET): todo.cpp
	$(CC) $(CFLAGS) todo.cpp -o $(TARGET) $(LDFLAGS)

$(DAEMON): todo_daemon.cpp
	$(CC) $(CFLAGS) todo_daemon.cpp -o $(DAEMON) $(LDFLAGS)

install:
	@bash ./setup.sh felix

clean:
	rm -f $(TARGET) $(DAEMON)
