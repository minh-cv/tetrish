#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

int main() {
    struct utsname uts;
    uname(&uts);
    printf("OS: %s\n", uts.sysname);       // "Linux"
    printf("Kernel: %s\n", uts.release);   // "5.15.0-..."
    printf("Hostname: %s\n", uts.nodename);

    FILE* f = fopen("/proc/meminfo", "r");
    char label[64];
    long value;
    fscanf(f, "%s %ld", label, &value);  // first line is MemTotal
    printf("Memory: %ld kB\n", value);
    fclose(f);

    printf("User: %s\n", getenv("USER"));

    f = fopen("/proc/cpuinfo", "r");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0) {
            printf("CPU: %s", strchr(line, ':') + 2);
            break;
        }
    }
    fclose(f);

    f = fopen("/proc/uptime", "r");
    double uptime;
    fscanf(f, "%lf", &uptime);
    printf("Uptime: %.0f seconds\n", uptime);
    fclose(f);
}