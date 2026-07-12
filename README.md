# sysseer

Windows system profiler + live resource monitor via HTTP API.

Inspired by the fact that Windows has no decent CLI system monitor that
doesnt require installing 200mb of .NET framework. This one fits in 8kb
of compiled C and runs on anything from XP to 11.

## endpoints

| path | description |
|------|-------------|
| / | system overview (host, os, cpu, ram, disks) |
| /ps | list running processes |
| /ps/verbose | same with memory usage per process |
| /net | show network interfaces and gateway |
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

## why

i got tired of opening task manager every time i want to see what's eating
my ram. task manager also lies about cpu usage half the time.

this uses the same NT api calls that task manager uses, so it's either as
accurate or equally wrong. pick your poison.
