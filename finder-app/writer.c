#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int writer(char *file_path, char *write_string){
    int fd, ret;

    fd = creat(file_path, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if(fd == -1){
        syslog(LOG_ERR, "Failed to open file %s: %s\n", file_path, strerror(errno));
        return 1;
    }

    ret = write(fd, write_string, strlen(write_string));
    if(ret == -1){
        syslog(LOG_ERR, "Failed to write \"%s\": %s\n", write_string, strerror(errno));
        return 2;
    }
    else if(ret < strlen(write_string)){
        syslog(LOG_ERR, "Only wrote %d chars of \"%s\"\n", ret, write_string);
        return 3;
    }

    syslog(LOG_DEBUG, "Writing %s to %s", write_string, file_path);

    if(close(fd) == -1){
        syslog(LOG_ERR, "Failed to close %s: %s\n", file_path, strerror(errno));
        return 4;
    }

    return 0;
}

int main(int argc, char **argv) {
    int ret;

    openlog("writer_log", LOG_CONS | LOG_NDELAY | LOG_PERROR, LOG_USER);

    if(argc != 3){
        syslog(LOG_ERR, "Invalid number of parameters. Usage:\n\t./writer [file_path] [write_string]\n");
        closelog();
        exit(1);
    }

    ret = writer(argv[1], argv[2]);

    closelog();
    exit(ret); 
}