#include <iostream>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
using namespace std;

#define MAX_LINE_LEN 15000
#define MAX_COMMAND_NUM 5000
#define MAX_COMMAND_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPEFD_NUM 200
#define MAX_FILENAME_LEN 1000

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;

struct CommandTable {
    char *commands[MAX_COMMAND_NUM];
    int length = 0;
};
struct ParsedCommand {
    int length = 0;
    char *argv[MAX_ARGV_LEN];
    char filename[MAX_FILENAME_LEN];
};
// pipefd[0] is read end, pipefd[1] is write end, count is for |num and !num, to_next=1 indicate '|' to_next=0 indicate "|num" or "!num".
struct Pipefd {
    int pipefd[2]; 
    int count;
    bool to_next;
};

void ChildHandler(int signo);
void SplitString(char *buffer, CommandTable &command_table);
void ParseCommand(CommandTable command_table, int &op, int &count, int &index, ParsedCommand &command_list);
void GetPath(ParsedCommand command_list, char command_path[]);
void Execute(ParsedCommand command_list, const char *command_path, pid_t pid_table[], int &pid_length, int inputfd, int outputfd, int errorfd, int op);
int GetInputfd(Pipefd pipefd_table[], int pipefd_length);
int GetOutputfd(Pipefd pipefd_table[], int &pipefd_length, int op, int count, ParsedCommand command_list);
int GetErrorfd(Pipefd pipefd_table[], int &pipefd_length, int op, int count);
void CountdownPipefd(Pipefd pipefd_table[], const int pipefd_length);
void CreatePipefd(Pipefd pipefd_table[], int &pipefd_length, int count, bool to_next);
void ClosePipefd(Pipefd pipefd_table[], int &pipefd_length);

int main() {
    char buffer[MAX_LINE_LEN] = {'\0'};
    int pipefd_length = 0;
    Pipefd pipefd_table[MAX_PIPEFD_NUM];

    setenv("PATH", "bin:.", 1);

    // receive inputs
    while (true) {
        cout << "% ";
        if (!cin.getline(buffer, sizeof(buffer))) {
            break;
        }
        
        CommandTable command_table;
        command_table.length = 0;
        pid_t pid_table[MAX_COMMAND_NUM];
        int pid_length = 0;
        int index = 0;

        CountdownPipefd(pipefd_table, pipefd_length);

        SplitString(buffer, command_table);

        // execute commands
        while (index < command_table.length) {
            int op = NONE, count = 0;
            int inputfd, outputfd, errorfd;
            char command_path[MAX_COMMAND_LEN];
            ParsedCommand command_list;
            
            ParseCommand(command_table, index, op, count, command_list);

            inputfd = GetInputfd(pipefd_table, pipefd_length);
            outputfd = GetOutputfd(pipefd_table, pipefd_length, op, count, command_list);
            errorfd = GetErrorfd(pipefd_table, pipefd_length, op, count);

            GetPath(command_list, command_path);
            Execute(command_list, command_path, pid_table, pid_length, inputfd, outputfd, errorfd, op);

            ClosePipefd(pipefd_table, pipefd_length);
        }
    }
    return 0;
}

void ChildHandler(int signo) {
    // polling to wait child process, WNOHANG indicate with no hang
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {

    }
}

void SplitString(char *buffer, CommandTable &command_table) {
    char delim[] = " ";
    char *pch = strtok(buffer, delim);

    while (pch != NULL) {
        command_table.commands[command_table.length++] = pch;
        pch = strtok(NULL, delim);
    }
}

void ParseCommand(CommandTable command_table, int &index, int &op, int &count, ParsedCommand &command_list) {
    command_list.length = 0;
    command_list.argv[command_list.length++] = command_table.commands[index++];
    while (index < command_table.length) {
        if (command_table.commands[index][0] == '|') {
            if (command_table.commands[index][1] != '\0') {
                op = NUMBER_PIPE;
                count = atoi(&command_table.commands[index][1]);
                break;
            } else {
                op = ORDINARY_PIPE;
                count = 0;
                break;
            }
        } else if (command_table.commands[index][0] == '!') {
            op = NUMBER_PIPE_ERR;
            count = atoi(&command_table.commands[index][1]);
            break;
        } else if (command_table.commands[index][0] == '>') {
            op = FILE_REDIRECTION;
            strcpy(command_list.filename, command_table.commands[++index]);
            break;
        } else {
            command_list.argv[command_list.length++] = command_table.commands[index];
        }
        index++;
    }

    index++;
    command_list.argv[command_list.length] = NULL;
}

void GetPath(ParsedCommand command_list, char command_path[]) {
    if (!strcmp(command_list.argv[0], "exit")) {
        strcpy(command_path, "exit");
        return;
    }else if (!strcmp(command_list.argv[0], "printenv")) {
        strcpy(command_path, "printenv");
        return;
    } else if (!strcmp(command_list.argv[0], "setenv")) {
        strcpy(command_path, "setenv");
        return;
    } else {
        char *env = getenv("PATH");
        char myenv[MAX_COMMAND_LEN];
        strcpy(myenv, env);
        char delim[] = ":";
        char *pch = strtok(myenv, delim);

        while (pch != NULL) {
            strcpy(command_path, pch);
            FILE *fp = fopen(strcat(strcat(command_path, "/"), command_list.argv[0]), "r");
            if (fp) {
                fclose(fp);
                return;
            }
            pch = strtok(NULL, delim);
        }
    }
    strcpy(command_path, "");
}

void Execute(ParsedCommand command_list, const char *command_path, pid_t pid_table[], int &pid_length, int inputfd, int outputfd, int errorfd, int op) {
    if (!strcmp(command_path, "exit")) {
        exit(0);
    } else if (!strcmp(command_path, "setenv")) {
        setenv(command_list.argv[1], command_list.argv[2], 1);
    } else if (!strcmp(command_path, "printenv")) {
        char *msg = getenv(command_list.argv[1]);
        if (msg) {
            cout << msg << endl;
        }
    } else {
        // ChildHandler get the zombie process
        signal(SIGCHLD, ChildHandler);
        pid_t pid;

        pid = fork();
        // repeat until seccessful fork
        while (pid < 0) {
            int status;
            waitpid(-1, &status, 0);
            pid = fork();
        }
        if (pid == 0) {
            if (inputfd != STDIN_FILENO) {
                dup2(inputfd, STDIN_FILENO);
            }
            if (outputfd != STDOUT_FILENO) {
                dup2(outputfd, STDOUT_FILENO);
            }
            if (errorfd != STDERR_FILENO) {
                dup2(errorfd, STDERR_FILENO);
            }

            if (inputfd != STDIN_FILENO) {
                close(inputfd);
            }
            if (outputfd != STDOUT_FILENO) {
                close(outputfd);
            }
            if (errorfd != STDERR_FILENO) {
                close(errorfd);
            }

            if (!strcmp(command_path, "")) {
                cerr << "Unknown command: [" << command_list.argv[0] << "]." << endl;
            } else {
                execvp(command_path, command_list.argv);
            }

            exit(0);
        } else {
            if (op == NONE || op == FILE_REDIRECTION) {
                pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                if (op == FILE_REDIRECTION) {
                    close(outputfd);
                }
            } else {
                pid_table[pid_length++] = pid;
            }
        }
    }
}

int GetInputfd(Pipefd pipefd_table[], int pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        // if pipe[i] is "|num" or "!num", and count=0, then close the write end.
        if (pipefd_table[i].count == 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].pipefd[1]);
            pipefd_table[i].pipefd[1] = -1;

            return pipefd_table[i].pipefd[0];
        }
    }

    return STDIN_FILENO;
}

int GetOutputfd(Pipefd pipefd_table[], int &pipefd_length, int op, int count, ParsedCommand command_list) {
    if (op == NUMBER_PIPE || op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            // use same count pipe which has used
            if (pipefd_table[i].count == count && pipefd_table[i].to_next == false) {
                return pipefd_table[i].pipefd[1];
            }
        }
        CreatePipefd(pipefd_table, pipefd_length, count, false);
        return pipefd_table[pipefd_length - 1].pipefd[1];
    } else if (op == ORDINARY_PIPE) {
        CreatePipefd(pipefd_table, pipefd_length, count, true);
        return pipefd_table[pipefd_length - 1].pipefd[1];
    } else if (op == FILE_REDIRECTION) {
        int fd = open(command_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        return fd;
    }
    
    return STDOUT_FILENO;
}

int GetErrorfd(Pipefd pipefd_table[], int &pipefd_length, int op, int count) {
    if (op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            if (pipefd_table[i].count == count && pipefd_table[i].to_next == false) {
                return pipefd_table[i].pipefd[1];
            }
        }
        CreatePipefd(pipefd_table, pipefd_length, count, false);
        return pipefd_table[pipefd_length - 1].pipefd[1];
    }
    
    return STDERR_FILENO;
}

void CountdownPipefd(Pipefd pipefd_table[], const int pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        pipefd_table[i].count -= 1;
    }
}

void CreatePipefd(Pipefd pipefd_table[], int &pipefd_length, int count, bool to_next) {
    pipe(pipefd_table[pipefd_length].pipefd);
    pipefd_table[pipefd_length].count = count;
    pipefd_table[pipefd_length].to_next = to_next;
    pipefd_length++;
}

void ClosePipefd(Pipefd pipefd_table[], int &pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].count <= 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].pipefd[0]);
            pipefd_table[i].pipefd[0] = -1;

            pipefd_length--;

            if (pipefd_length > 0) {
                Pipefd temp = pipefd_table[pipefd_length];
                pipefd_table[pipefd_length] = pipefd_table[i];
                pipefd_table[i] = temp;
            }

            i--;
        } else if (pipefd_table[i].to_next == true) {
            pipefd_table[i].to_next = false;
        }
    }
}