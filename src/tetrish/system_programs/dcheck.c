#include <stdio.h>

int main(void) {
    const char *cmd = "ps -efj | grep dspawn | grep -Ev 'tty|pts'";

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    char buf[1024];
    while (fgets(buf, sizeof buf, fp)) {
        fputs(buf, stdout);   // print each line as we read it
    }


    int status = pclose(fp);
    if (status == -1) {
        perror("pclose");
        return -1;
    }
}