#! /bin/sh
# Simple script to start/stop our socket server as a daemon, adapted from code
# provided in class and mastering embedded linux programming chapter 10
# Usage:
#   ./aesdsocket-start-stop {start|stop}

case "$1" in
    start)
        echo "Starting aesdsocket in daemon mode"
        start-stop-daemon -S -n aesdsocket -x /usr/bin/aesdsocket
        ;;
    stop)
        echo "Stopping aesdsocket"
        start-stop-daemon -K -n aesdsocket
        ;;
    *)
        echo "Usage: $0 {start|stop}"
    exit 1
esac

exit 0