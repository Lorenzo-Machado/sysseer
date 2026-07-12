# sysseer

Windows system profiler + live resource monitor via HTTP API. Written in C, no dependencies, compiles to a standalone binary under 10KB.

Inspired by the fact that Windows has no decent CLI system monitor that doesnt require installing 200mb of .NET framework.

## endpoints

| path | description |
|------|-------------|
| / | HTML dashboard with live auto-refresh (1s) |
| /ps | list running processes |
| /ps/verbose | processes with memory usage |
| /net | network interfaces and gateway |
| /monitor | live cpu/memory/disk usage |

## building

needs mingw-w64 or visual studio build tools:

```sh
x86_64-w64-mingw32-gcc -O2 -o sysseer.exe sysseer.c -lws2_32 -liphlpapi -lpsapi
```

or open in visual studio and build.

## usage

```sh
sysseer.exe
```

then open http://localhost:9090 in browser or use curl:

```sh
curl http://localhost:9090
curl http://localhost:9090/ps
curl http://localhost:9090/monitor
```

## v2 changelog

- HTML dashboard at / with auto-refresh every 1s via JavaScript

- bind to loopback only (127.0.0.1) instead of INADDR_ANY
- socket timeouts (5s) to prevent hanging on slow clients
- request size limit to prevent resource exhaustion
- reject non-GET methods
- strip query strings from path before routing
- proper error checking for all Win32 API calls
- snprintf everywhere with bounds checking, no buffer overflow risks
- critical section for thread-safe CPU monitoring
- CPU monitor returns a value on first call instead of zero
- refactored with helper functions (safe_send, send_response)
- connection backlog limited to 10
- handle WSAEINTR on accept to prevent crashes
- X-Content-Type-Options: nosniff on all responses
- GetDiskFreeSpaceEx checks total > 0 before dividing

## why

i got tired of opening task manager every time i want to see what's eating my ram. task manager also lies about cpu usage half the time.

this uses the same NT api calls that task manager uses, so it's either as accurate or equally wrong. pick your poison.
