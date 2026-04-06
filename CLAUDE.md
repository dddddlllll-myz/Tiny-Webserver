# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make        # Build the server binary
make clean  # Remove the binary
./build.sh  # Alias for make
```

## Running the Server

```bash
./server -p 9006 -l 0 -m 0 -o 0 -s 8 -t 8 -c 0 -a 0
```

Command line arguments:
- `-p` port (default 9006)
- `-l` log write mode: 0=sync, 1=async (default 0)
- `-m` trigger mode: 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET (default 0)
- `-o` option linger: 0=graceful, 1=force (default 0)
- `-s` SQL connection pool size (default 8)
- `-t` thread pool size (default 8)
- `-c` close log: 0=no, 1=yes (default 0)
- `-a` actor model: 0=proactor, 1=reactor (default 0)

## Architecture

### Event Loop (Webserver::eventLoop)
Main loop uses epoll_wait. Handles: new connections, I/O events (EPOLLIN/EPOLLOUT), signals via pipe, and timer tick() on SIGALRM.

### Reactor vs Proactor
- **Proactor** (default): main loop reads data into buffer, then hands to thread pool for processing
- **Reactor**: thread pool does the read itself via non-blocking I/O with EPOLLONESHOT

### HTTP State Machine
Http_Conn uses a main state machine (CHECK_STATE_REQUESTLINE → HEADER → CONTENT) and a sub-state machine (LINE_OK/LINE_BAD/LINE_OPEN) to parse HTTP requests. The process_read()/process_write() pair handles request/response.

### Key Classes
- `Webserver`: main server class, owns epoll fd, timer list, thread pool, SQL pool
- `Http_Conn`: one per client connection, handles HTTP parsing and response
- `Thread_Pool<Http_Conn>`: worker pool, processes Http_Conn requests
- `Sort_Timer_List`: sorted linked list of Util_Timer, triggered by SIGALRM
- `Conn_Pool`: MySQL connection pool (singleton), uses RAII (ConnectionRAII)
- `Log`: singleton, supports sync and async (block queue) logging

### Database Login Flow
1. Parse user/password from HTTP POST body
2. Get connection from pool via ConnectionRAII
3. Query: `SELECT password FROM user WHERE username = 'xxx'`
4. Compare hash with input
5. Release connection (RAII destructor)

### File Structure
- Root dir: main.cpp, Webserver.h/cpp, Config.h/cpp, makefile
- Http/: HTTP connection handling (Http_Conn.h/cpp)
- Thread_Pool/: thread pool template
- Timer/: sorted timer list, Utils class for signals
- CGI_MySql/: MySQL connection pool
- Log/: async-safe logging with block queue
- Lock/: semaphores and mutex wrappers
- Root/: static web content (html, images, video)