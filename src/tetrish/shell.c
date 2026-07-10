// This code was written by ChatGPT4
// Modify it for your own usage to implement features for PA1 (or completely
// rewrite it) Include the shell header file for necessary constants and
// function declarations
#include "libs/shell.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int shell_cd(char **args) {
    char* path = args[1] == NULL ? getenv("HOME") : args[1];
    if (chdir(path) != 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int shell_help(char **args) {
    (void)args;
    printf(
"The following commands are implemented within the shell:\n\
    cd\n\
    help\n\
    exit\n\
    usage\n\
    env\n\
    setenv\n\
    unsetenv\n"
    );

    return 0;
}

static int shell_exit(char **cmd) {
    for (int i = 0; cmd[i] != NULL && i < MAX_ARGS; i++) {
        free(cmd[i]);
    }
    exit(0);
}

static int shell_usage(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: usage <command>.\n");
        return 1;
    }

    size_t cmd_idx = get_builtin_command_index(args[1]);
    switch (cmd_idx) {
    case 0: // cd
        printf("cd [<directory>]\nChange the current working directory of the shell. Defaults to $HOME.\n");
        break;
    case 1: // help
        printf("help\nList all builtin commands.\n");
        break;
    case 2: // exit
        printf("exit\nTerminate the shell.\n");
        break;
    case 3: // usage
        printf("usage <cmd>\nPrint how to use a command.\n");
        break;
    case 4: // env
        printf("env\nList all environment variables.\n");
        break;
    case 5: // setenv
        printf("setenv <env-name>=<value>\nSet an environment variable.\n");
        break;
    case 6: // unsetenv
        printf("unsetenv <env-name>\nUnset an environment variable.\n");
        break;
    default:
        fprintf(stderr, "Non-builtin command not supported.\n");
        return 2;
    }
    return 0;
}

static int list_env(char **args) {
    (void)args;

    extern char** environ;
    char** env = environ;

    while (*env != NULL) {
        printf("%s\n", *env);
        env++;
    }

    return 0;
}

int set_env_var(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: setenv <env-name>=<value>\n");
        return 1;
    }
    char* eq_at = strchr(args[1], '=');
    if (eq_at == NULL ||
        eq_at == args[1] // no name
    ) {
        fprintf(stderr, "Usage: setenv <env-name>=<value>\n");
        return 1;
    }
    *eq_at = '\0';
    int return_val = setenv(args[1], eq_at + 1, 1);
    *eq_at = '=';
    if (return_val != 0) {
        perror("setenv");
        return 1;
    }
    return return_val;
}

static int unset_env_var(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: unsetenv <env-name>\n");
        return 1;
    }
    int retval = unsetenv(args[1]);
    if (retval != 0) {
        perror("unsetenv");
        return 1;
    }
    return retval;
}

static const char *builtin_commands[] = {
    "cd",   // Changes the current directory of the shell to the specified path.
            // If no path is given, it defaults to the user's home directory.
    "help", //  List all builtin commands in the shell
    "exit", // Exits the shell
    "usage",  // Provides a brief usage guide for the shell and its built-in
              // command
    "env",    // Lists all the environment variables currently set in the shell
    "setenv", // Sets or modifies an environment variable for this shell session
    "unsetenv" // Removes an environment variable from the shell
};

static int (*builtin_command_func[])(char **) = {
    &shell_cd,     // builtin_command_func[0]: cd
    &shell_help,   // builtin_command_func[1]: help
    &shell_exit,   // builtin_command_func[2]: exit
    &shell_usage,  // builtin_command_func[3]: usage
    &list_env,     // builtin_command_func[4]: env
    &set_env_var,  // builtin_command_func[5]: setenv
    &unset_env_var // builtin_command_func[6]: unsetenv
};

size_t get_builtin_command_index(char *cmd) {
    for (size_t i = 0; i < sizeof(builtin_commands)/sizeof(const char*); i++) {
        if (strcmp(cmd, builtin_commands[i]) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

void execute_builtin_command(char **cmd, size_t index) {
    builtin_command_func[index](cmd);
}

// Function to read a command from the user input
bool read_command_from_file(char **cmd, FILE* file) {
  // Define a character array to store the command line input
  char line[MAX_LINE];
  // Initialize count to keep track of the number of characters read
  int count = 0, i = 0;
  // Array to hold pointers to the parsed command arguments
  char *array[MAX_ARGS], *command_token;

  // Infinite loop to read characters until a newline or maximum line length is
  // reached
  for (;;) {
    // Read a single character from standard input
    int current_char = fgetc(file);
    // Store the character in the line array and increment count
    line[count++] = (char)(current_char == EOF ? '\n' : current_char);
    // If a newline character is encountered, break out of the loop
    if (current_char == '\n' || current_char == EOF)
      break;
    // If the command exceeds the maximum length, print an error and exit
    if (count >= MAX_LINE) {
      printf("Command is too long, unable to process\n");
      exit(1);
    }
  }
  // Null-terminate the command line string
  line[count] = '\0';

  // If only the newline character was entered, return without processing
  if (count == 1)
    return feof(file) == 0;

  // Use strtok to parse the first token (word) of the command
  command_token = strtok(line, " \n");

  // Continue parsing the line into words and store them in the array
  while (command_token != NULL) {
    array[i++] = strdup(command_token);  // Duplicate the token and store it
    command_token = strtok(NULL, " \n"); // Get the next token
  }

  // Copy the parsed command and its parameters to the cmd array
  for (int j = 0; j < i; j++) {
    cmd[j] = array[j];
  }
  // Null-terminate the cmd array to mark the end of arguments
  cmd[i] = NULL;
  return feof(file) == 0;
}

bool read_command(char **cmd) {
    return read_command_from_file(cmd, stdin);
}

// Function to display the shell prompt
void type_prompt() {
  fflush(stdout); // Flush the output buffer
  printf("$$ ");  // Print the shell prompt
}

void clear() {
#ifdef _WIN32
    system("cls"); // Windows command to clear screen
#else
    system("clear"); // UNIX/Linux command to clear screen
#endif

}
