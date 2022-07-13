#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>

using namespace std;

#define MAX_LINE_LEN 15000
#define MAX_COMMAND_NUM 5000
#define MAX_COMMAND_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_FILENAME_LEN 1000
#define MAX_PIPEFD_NUM 1024
#define ENV_NUM 100
#define ENV_LEN 500

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;

struct Pipefd {
    int IOfd[2];
    int count;
    bool to_next;
};

struct CmdTable {
    char *commands[MAX_COMMAND_NUM];
    int length;
};

struct ParsedCmd {
    int length;
    int op;
    int count;
    char *argv[MAX_ARGV_LEN];
    char filename[MAX_FILENAME_LEN];
};

struct EnvTable {
    char key[ENV_NUM][ENV_LEN];
    char value[ENV_NUM][ENV_LEN];
    int length;
};

int TCPconnect(uint16_t port);
void ChildHandler(int signo);
void ExecSocket(int client_sockfd, EnvTable &env);
void SplitString(char *input, CmdTable &cmd_table);
bool IsBuildInCmd(CmdTable cmd_table, EnvTable &env, int sockfd);
void ParseCmd(CmdTable cmd_table, int &index, ParsedCmd &cmd_list);
void GetPath(ParsedCmd cmd_list, char command_path[], EnvTable env);
void ExecCmd(ParsedCmd cmd_list, const char *command_path, pid_t pid_table[], int &pid_length, int stdIOfd[]);
void CountdownPipefd(Pipefd pipefd_table[], int pipefd_length);
void CreatePipefd(Pipefd pipefd_table[], int &pipefd_length, ParsedCmd cmd_list, bool to_next);
void GetStdIOfd(Pipefd pipefd_table[], int &pipefd_length, int sockfd, ParsedCmd cmd_list, int stdIOfd[]);
void ClosePipefd(Pipefd pipefd_table[], int &pipefd_length);

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_simple [port]" << endl;
        exit(1);
    }

    int server_sockfd, client_sockfd, port;
    socklen_t addr_len;
    sockaddr_in client_addr;

    port = atoi(argv[1]);
    server_sockfd = TCPconnect(port);
    // cout << "server sockfd: " << server_sockfd << endl;

    while (true) {
        addr_len = sizeof(client_addr);
        client_sockfd = accept(server_sockfd, (sockaddr *) &client_addr, &addr_len);
        if (client_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }
        // cout << "client sockfd: " << client_sockfd << endl;

        EnvTable env = {
            .length = 0
        };
        strcpy(env.key[env.length], "PATH");
        strcpy(env.value[env.length], "bin:.");
        env.length++;

        ExecSocket(client_sockfd, env);

        close(client_sockfd);
    }
    close(server_sockfd);
    return 0;
}

int TCPconnect(uint16_t port) {
    int sockfd, optval = 1;
    sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        cerr << "Error: socket failed" << endl;
        exit(1);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        cerr << "Error: setsockopt failed" << endl;
        exit(1);
    }
    if (bind(sockfd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Error: bind failed" << endl;
        exit(1);
    }
    if (listen(sockfd, 0) < 0) {
        cerr << "Error: listen failed" << endl;
        exit(1);
    }

    return sockfd;
}

void ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

void ExecSocket(int sockfd, EnvTable &env) {
    char input[MAX_LINE_LEN];
    memset(&input, '\0', sizeof(input));
    int pipefd_length = 0;
    Pipefd pipefd_table[MAX_PIPEFD_NUM];

    while (true) {
        CmdTable cmd_table;
        char read_buffer[MAX_LINE_LEN];
        pid_t pid_table[MAX_COMMAND_NUM];
        int pid_length = 0;
        int index = 0;

        write(sockfd, "% ", strlen("% "));
        
        memset(&input, '\0', sizeof(input));
        do {
            memset(&read_buffer, '\0', sizeof(read_buffer));
            read(sockfd, read_buffer, sizeof(read_buffer));
            strcat(input, read_buffer);
        } while (read_buffer[strlen(read_buffer) - 1] != '\n');

        strtok(input, "\r\n");

        if (!strcmp(input, "")) {
            continue;
        }
        if (!strcmp(input, "exit")) {
            return;
        }

        CountdownPipefd(pipefd_table, pipefd_length);

        SplitString(input, cmd_table);

        while (index < cmd_table.length) {
            if (!IsBuildInCmd(cmd_table, env, sockfd)) {
                int stdIOfd[3];
                ParsedCmd cmd_list;
                char command_path[MAX_COMMAND_LEN];

                ParseCmd(cmd_table, index, cmd_list);

                GetStdIOfd(pipefd_table, pipefd_length, sockfd, cmd_list, stdIOfd);

                GetPath(cmd_list, command_path, env);
                ExecCmd(cmd_list, command_path, pid_table, pid_length, stdIOfd);
                ClosePipefd(pipefd_table, pipefd_length);
            } else {
                break;
            }
        }
    }
}

void SplitString(char *input, CmdTable &cmd_table) {
    cmd_table.length = 0;
    char delim[] = " \n";
    char *pch = strtok(input, delim);

    while (pch != NULL) {
        cmd_table.commands[cmd_table.length++] = pch;
        pch = strtok(NULL, delim);
    }
}

bool IsBuildInCmd(CmdTable cmd_table, EnvTable &env, int sockfd) {
    if (!strcmp(cmd_table.commands[0], "setenv")) {
        for (int i = 0; i < env.length; ++i) {
            if (!strcmp(env.key[i], cmd_table.commands[1])) {
                strcpy(env.value[i], cmd_table.commands[2]);
                return true;
            }
        }
        strcpy(env.key[env.length], cmd_table.commands[1]);
        strcpy(env.value[env.length], cmd_table.commands[2]);
        env.length++;
        return true;

    } else if (!strcmp(cmd_table.commands[0], "printenv")) {
        for (int i = 0; i < env.length; ++i) {
            if (!strcmp(env.key[i], cmd_table.commands[1])) {
                write(sockfd, env.value[i], strlen(env.value[i]));
                write(sockfd, "\n", strlen("\n"));
                return true;
            }
        }
        char *myenv = getenv(cmd_table.commands[1]);
        if (myenv) {
            write(sockfd, myenv, strlen(myenv));
        }
        write(sockfd, "\n", strlen("\n"));
        return true;
    }

    return false;
}

void ParseCmd(CmdTable cmd_table, int &index, ParsedCmd &cmd_list) {
    cmd_list.op = NONE;
    cmd_list.count = 0;

    cmd_list.length = 0;
    cmd_list.argv[cmd_list.length++] = cmd_table.commands[index++];
    while (index < cmd_table.length) {
        if (cmd_table.commands[index][0] == '|') {
            if (cmd_table.commands[index][1] != '\0') {
                cmd_list.count = atoi(&cmd_table.commands[index][1]);
                cmd_list.op = NUMBER_PIPE;
                break;
            } else {
                cmd_list.count = 0;
                cmd_list.op = ORDINARY_PIPE;
                break;
            }
        } else if (cmd_table.commands[index][0] == '!') {
            cmd_list.count = atoi(&cmd_table.commands[index][1]);
            cmd_list.op = NUMBER_PIPE_ERR;
            break;
        } else if (cmd_table.commands[index][0] == '>') {
            strcpy(cmd_list.filename, cmd_table.commands[index + 1]);
            index++;
            cmd_list.op = FILE_REDIRECTION;
            break;
        } else {
            cmd_list.argv[cmd_list.length++] = cmd_table.commands[index];
        }
        index++;
    }

    cmd_list.argv[cmd_list.length] = NULL;
    index++;
}

void GetPath(ParsedCmd cmd_list, char command_path[], EnvTable env) {
    char myenv[MAX_COMMAND_LEN];
    strcpy(myenv, env.value[0]);
    char delim[] = ":";
    char *pch = strtok(myenv, delim);

    while (pch != NULL) {
        strcpy(command_path, pch);
        if (!access(strcat(strcat(command_path, "/"), cmd_list.argv[0]), X_OK)) {
            return;
        }
        pch = strtok(NULL, delim);
    }

    strcpy(command_path, "");
}

void ExecCmd(ParsedCmd cmd_list, const char *command_path, pid_t pid_table[], int &pid_length, int stdIOfd[]) {
    if (!strcmp(command_path, "setenv")) {
        setenv(cmd_list.argv[1], cmd_list.argv[2], 1);
    } else if (!strcmp(command_path, "printenv")) {
        char *msg = getenv(cmd_list.argv[1]);
        char output[3000];
        sprintf(output, "%s\n", msg);
        if (msg) {
            write(stdIOfd[1], output, strlen(output));
        }
    } else {
        signal(SIGCHLD, ChildHandler);
        pid_t pid;

        pid = fork();
        while (pid < 0) {
            int status;
            waitpid(-1, &status, 0);
            pid = fork();
        }
        if (pid == 0) {
            dup2(stdIOfd[0], STDIN_FILENO);
            dup2(stdIOfd[1], STDOUT_FILENO);
            dup2(stdIOfd[2], STDERR_FILENO);

            if (stdIOfd[0] != STDIN_FILENO) {
                close(stdIOfd[0]);
            }
            if (stdIOfd[1] != STDOUT_FILENO) {
                close(stdIOfd[1]);
            }
            if (stdIOfd[2] != STDERR_FILENO) {
                close(stdIOfd[2]);
            }

            if (!strcmp(command_path, "")) {
                cerr << "Unknown command: [" << cmd_list.argv[0] << "]." << endl;
            } else {
                while (execvp(command_path, cmd_list.argv) == -1) {
                    cerr << "error exec" << endl;
                };
            }

            exit(0);
        } else {
            pid_table[pid_length++] = pid;
            if (cmd_list.op == NONE || cmd_list.op == FILE_REDIRECTION) {
                pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                if (cmd_list.op == FILE_REDIRECTION) {
                    close(stdIOfd[1]);
                }
            }
        }
    }
}

void CountdownPipefd(Pipefd pipefd_table[], const int pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        pipefd_table[i].count -= 1;
    }
}

void CreatePipefd(Pipefd pipefd_table[], int &pipefd_length, ParsedCmd cmd_list, bool to_next) {
    pipe(pipefd_table[pipefd_length].IOfd);
    pipefd_table[pipefd_length].count = cmd_list.count;
    pipefd_table[pipefd_length].to_next = to_next;
    pipefd_length++;
}

void GetStdIOfd(Pipefd pipefd_table[], int &pipefd_length, int sockfd, ParsedCmd cmd_list, int stdIOfd[]) {
    bool find = false;
    for (int i = 0; i < pipefd_length; ++i) {
        // if pipe[i] is "|num" or "!num", and count=0, then close the write end.
        if (pipefd_table[i].count == 0) {
            close(pipefd_table[i].IOfd[1]);
            pipefd_table[i].IOfd[1] = -1;

            stdIOfd[0] =  pipefd_table[i].IOfd[0];
            find = true;
            break;
        }
    }
    if (!find) stdIOfd[0] = STDIN_FILENO;

    find = false;
    if (cmd_list.op == NUMBER_PIPE || cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            // use same count pipe which has used
            if (pipefd_table[i].count == cmd_list.count) {
                stdIOfd[1] = pipefd_table[i].IOfd[1];
                find = true;
                break;
            }
        }
        if (!find){
            CreatePipefd(pipefd_table, pipefd_length, cmd_list, false);
            stdIOfd[1] = pipefd_table[pipefd_length - 1].IOfd[1];
            find = true;
        }
    } else if (cmd_list.op == ORDINARY_PIPE) {
        CreatePipefd(pipefd_table, pipefd_length, cmd_list, true);
        stdIOfd[1] = pipefd_table[pipefd_length - 1].IOfd[1];
        find = true;
    } else if (cmd_list.op == FILE_REDIRECTION) {
        stdIOfd[1] = open(cmd_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        find = true;
    }
    if (!find) stdIOfd[1] = sockfd;

    find = false;
    if (cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            if (pipefd_table[i].count == cmd_list.count) {
                stdIOfd[2] = pipefd_table[i].IOfd[1];
                find = true;
                break;
            }
        }
        if(!find){
            CreatePipefd(pipefd_table, pipefd_length, cmd_list, false);
            stdIOfd[2] = pipefd_table[pipefd_length - 1].IOfd[1];
        }
    }
    if (!find) stdIOfd[2] = sockfd;
}


void ClosePipefd(Pipefd pipefd_table[], int &pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].count <= 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].IOfd[0]);
            
            pipefd_length -= 1;

            Pipefd temp = pipefd_table[pipefd_length];
            pipefd_table[pipefd_length] = pipefd_table[i];
            pipefd_table[i] = temp;

            i -= 1;
        } else if (pipefd_table[i].to_next == true) {
            pipefd_table[i].to_next = false;
        }
    }
}

void ClearPipefd(Pipefd pipefd_table[], int &pipefd_length) {
    for (int i = 0; i < pipefd_length; ++i) {
        close(pipefd_table[i].IOfd[0]);
        close(pipefd_table[i].IOfd[1]);
    }
    pipefd_length = 0;
}