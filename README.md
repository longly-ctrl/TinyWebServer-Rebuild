# TinyWebServer-Rebuild

A learning-oriented C++ TinyWebServer rebuild for Ubuntu 22.04. It implements the core ideas of the original TinyWebServer project in a compact single-file form.

## Features

- TCP server based on Linux socket APIs.
- I/O multiplexing with `epoll`.
- Non-blocking listening and client sockets.
- HTTP request parsing with a finite-state flow.
- Static file serving from `root/`.
- `POST /login` and `POST /register` form handling.
- MySQL 8.0 authentication table.
- Thread pool for business processing.
- MySQL connection pool with RAII connection return.
- Basic synchronized logging.
- Idle connection cleanup.
- Basic command-line options.

## Build

Install dependencies on Ubuntu 22.04:

```bash
sudo apt update
sudo apt install -y g++ make mysql-server libmysqlclient-dev curl net-tools
```

Build:

```bash
make clean
make
```

## MySQL Setup

```sql
CREATE DATABASE yourdb DEFAULT CHARACTER SET utf8mb4;
USE yourdb;

CREATE TABLE user (
    username VARCHAR(50) NOT NULL PRIMARY KEY,
    passwd VARCHAR(50) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE USER 'tinyweb'@'localhost' IDENTIFIED BY '123123';
GRANT ALL PRIVILEGES ON yourdb.* TO 'tinyweb'@'localhost';
FLUSH PRIVILEGES;
```

## Run

Default:

```bash
./server
```

With options:

```bash
./server -p 9006 -t 8 -s 8 -r ./root -o 30
```

Options:

- `-p`: server port.
- `-t`: worker thread count.
- `-s`: MySQL connection count.
- `-r`: static root path.
- `-o`: idle timeout seconds.

Visit:

```text
http://192.168.220.128:9006/index.html
```

Use your actual VM IP if it changes.

## Test

```bash
curl http://127.0.0.1:9006/index.html
curl -X POST http://127.0.0.1:9006/register -d "username=tom&password=123123"
curl -X POST http://127.0.0.1:9006/login -d "username=tom&password=123123"
```

## Interview Hotspots

### 1. TCP Server Startup

Know the sequence:

```text
socket -> bind -> listen -> accept
```

Explain what each step does and why `SO_REUSEADDR` helps restart the server quickly.

### 2. Blocking vs Non-blocking I/O

Blocking I/O waits until the operation completes.

Non-blocking I/O returns immediately and may report `EAGAIN` or `EWOULDBLOCK`.

This project uses non-blocking sockets so one thread is not stuck waiting on one client.

### 3. epoll

`epoll` lets one thread monitor many file descriptors.

Important APIs:

```text
epoll_create
epoll_ctl
epoll_wait
```

Be able to explain why `epoll` is more suitable for high concurrency than one-thread-per-connection.

### 4. Reactor Model

The main thread waits for I/O events and dispatches work.

Worker threads process HTTP logic and database operations.

This is a simplified Reactor-style design.

### 5. Thread Pool

A thread pool avoids creating and destroying threads for every request.

Core components:

```text
worker threads
task queue
mutex
condition_variable
```

### 6. MySQL Connection Pool

Creating a database connection for every request is expensive.

A connection pool reuses existing connections.

Core operations:

```text
get_connection
release_connection
close_pool
```

### 7. RAII

RAII binds resource release to object lifetime.

In this project, `MysqlConnectionRAII` automatically returns a MySQL connection to the pool when the object goes out of scope.

### 8. HTTP Parsing

HTTP request structure:

```text
request line
headers
blank line
body
```

For POST, `Content-Length` determines how many bytes belong to the request body.

### 9. Static File Serving

The server maps URL paths to files under `root/`.

The path traversal check rejects `..` to prevent reading files outside the static root.

### 10. Login and Register

The frontend submits forms with:

```html
<form action="/login" method="post">
<form action="/register" method="post">
```

The backend parses `username` and `password`, escapes SQL input, and queries MySQL.

### 11. SQL Injection

User input cannot be directly inserted into SQL.

This project uses `mysql_real_escape_string` as a learning-stage mitigation.

In production, prepared statements are preferred.

### 12. Idle Connection Cleanup

Idle connections consume file descriptors.

The server records last active time and closes connections that exceed the timeout.

### 13. Common Follow-up Questions

- Why use `epoll` instead of `select`?
- What is the difference between LT and ET trigger modes?
- Why use a thread pool?
- Why use a database connection pool?
- What problem does RAII solve?
- How does HTTP distinguish headers from body?
- Why does POST need `Content-Length`?
- What is SQL injection?
- What happens if `write` only sends part of the response?
- How do you handle inactive clients?
