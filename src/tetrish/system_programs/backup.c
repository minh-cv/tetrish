#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main() {
    const char* BACKUP_DIR = getenv("BACKUP_DIR");
    if (BACKUP_DIR == NULL) {
        fprintf(stderr, "Error: BACKUP_DIR not set\n");
        return 1;
    }
    const char* archive_path = getenv("PROJECT_DIR");
    if (archive_path == NULL) {
        fprintf(stderr, "Error: PROJECT_DIR not set\n");
        return 1;
    }
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (tm_info == NULL) {
        perror("localtime");
        return 1;
    }

    char filename[PATH_MAX];
    int written_char = snprintf(filename, PATH_MAX,
         "%s/archive/backup-%04d-%02d-%02d-%02d-%02d-%02d.tar.gz",
         archive_path,
         tm_info->tm_year + 1900,
         tm_info->tm_mon + 1,
         tm_info->tm_mday,
         tm_info->tm_hour,
         tm_info->tm_min,
         tm_info->tm_sec);
    if (written_char < 0 || written_char >= (int)sizeof filename) {
        fprintf(stderr, "Error: archive path too long.\n");
        return 1;
    }

    int pid = fork();
    if (pid < 0) {
        perror("backup");
        return 1;
    }
    if (pid == 0) {
        execlp("tar", "tar", "-czvf", filename, BACKUP_DIR, NULL);
        perror("tar");
        _exit(1);
    }
    
    int status;
    wait(&status);
    if (wait(&status) == -1) {
        perror("wait");
        return 1;
    }

    if (!WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}