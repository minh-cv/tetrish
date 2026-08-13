#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static void print_meminfo(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        printf("Memory: unknown\n");
        return;
    }

    char label[64];
    long value;
    if (fscanf(f, "%63s %ld", label, &value) == 2) { // first line is MemTotal
        printf("Memory: %ld kB\n", value);
    } else {
        printf("Memory: unknown\n");
    }
    fclose(f);
}

static void print_cpu(void) {
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        printf("CPU: unknown\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0) {
            const char* colon = strchr(line, ':');
            if (colon != NULL && colon[1] != '\0') {
                printf("CPU: %s", colon + 2);
                fclose(f);
                return;
            }
        }
    }

    printf("CPU: unknown\n");
    fclose(f);
}

static void print_uptime(void) {
    FILE* f = fopen("/proc/uptime", "r");
    if (f == NULL) {
        printf("Uptime: unknown\n");
        return;
    }

    double uptime;
    if (fscanf(f, "%lf", &uptime) == 1) {
        printf("Uptime: %.0f seconds\n", uptime);
    } else {
        printf("Uptime: unknown\n");
    }
    fclose(f);
}

int main(void) {
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("OS: %s\n", uts.sysname);     // "Linux"
        printf("Kernel: %s\n", uts.release); // "5.15.0-..."
        printf("Hostname: %s\n", uts.nodename);
    } else {
        perror("uname");
    }

    print_meminfo();

    const char* user = getenv("USER");
    printf("User: %s\n", user != NULL ? user : "unknown");

    print_cpu();
    print_uptime();

    return 0;
}
