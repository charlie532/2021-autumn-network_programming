#include <cstdio>
#include <iostream>
#include <sys/select.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

using namespace std;

#define MAX_LINE_LEN 15000
#define MAX_COMMAND_NUM 5000
#define MAX_COMMAND_LEN 1000
#define MAX_FILENAME_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPEFD_NUM 1024
#define MAX_MSG_LEN 1024
#define MAX_NAME_LEN 20
#define MAX_USER 30
#define ENV_NUM 100
#define ENV_LEN 500

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;
const int USER_PIPE_OUT = 6;
const int USER_PIPE_NULL = 7;

struct EnvTable {
    char key[ENV_NUM][ENV_LEN];
    char value[ENV_NUM][ENV_LEN];
    int length;
};
struct User {
    char name[MAX_NAME_LEN];
    char ip_port[40];
    int sockfd;
    bool is_login;
    EnvTable env;
};
struct CmdTable {
    char *commands[MAX_COMMAND_NUM];
    int command_length;
};
struct ParsedCmd {
    int op;
    bool in_pipe;
    int from_user;
    int to_user;
    int count;
    int argc;
    char *argv[MAX_ARGV_LEN];
    char filename[MAX_FILENAME_LEN];
};
struct Pipefd {
    int IOfd[2];
    int count;
    int sockfd;
    bool to_next;
};
struct UserPipe {
    int IOfd[2];
    int from_user;
    int to_user;
};

User users[MAX_USER + 1];
Pipefd pipefd_table[MAX_PIPEFD_NUM];
UserPipe user_pipes[MAX_PIPEFD_NUM];
int pipefd_length, user_pipe_length;
char raw_command[MAX_COMMAND_LEN];

void ChildHandler(int signo);
int TCPconnect(uint16_t port);
void BroadcastMsg(const char *msg);
int LoginUser(fd_set &activefds, int server_sockfd);
void LoginBroadcast(User user);
void LogoutUser(fd_set &activefds, int sockfd);
void LogoutBroadcast(User user);
int GetUserIndex(int sockfd);
void WriteWLCMMsg(int sockfd);
bool IsBuildInCmd(int sockfd, CmdTable cmd_table);
void Yell(int sockfd, const char *msg);
void Name(int sockfd, const char *name);
void Who(int sockfd);
void Tell(int sockfd, int user_id, char *input);
void Printenv(int sockfd, CmdTable cmd_table);
void Setenv(int sockfd, CmdTable cmd_table);
void SplitString(char *input, CmdTable &cmd_table);
void ParseCmd(CmdTable cmd_table, int &index, ParsedCmd &command);
void GetCmdPath(ParsedCmd command, char command_path[], int sockfd);
void ExecCmd(ParsedCmd command, const char *command_path, pid_t pid_table[], int &pid_length, int stdIOfd[]);
void CountdownPipefd(int sockfd);
void CreatePipefd(int sockfd, ParsedCmd command, bool to_next);
int GetInputfd(int sockfd, ParsedCmd command);
int GetOutputfd(int sockfd, ParsedCmd &command);
int GetErrorfd(int sockfd, ParsedCmd command);
void CreateUserPipe(int from_user, int to_user);
int GetUserPipeIndex(int from_user, int to_user);
void ClosePipefd(int sockfd, int inputfd, ParsedCmd command);
void ClearPipefd(int sockfd);

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_single_proc [port]" << endl;
        exit(1);
    }

    fd_set readfds, activefds;
    int server_sockfd, port, fds_num;
    pipefd_length = 0;
    user_pipe_length = 0;

    for (int i = 1; i <= MAX_USER; ++i) {
        users[i].is_login = false;
    }

    port = atoi(argv[1]);
    server_sockfd = TCPconnect(port);

    // get fdtable size
    fds_num = getdtablesize();
    FD_ZERO(&activefds);
    FD_SET(server_sockfd, &activefds);

    while (true) {
        memcpy(&readfds, &activefds, sizeof(readfds));

        if (select(fds_num, &readfds, NULL, NULL, NULL) < 0) {
            cerr << "Error: select error" << endl;
            continue;
        }

        if (FD_ISSET(server_sockfd, &readfds)) {
            int client_sockfd;
            client_sockfd = LoginUser(activefds, server_sockfd);
            write(client_sockfd, "% ", strlen("% "));
        }
        for (int sockfd = 0; sockfd < fds_num; ++sockfd) {
            if (server_sockfd != sockfd && FD_ISSET(sockfd, &readfds)) {
                CmdTable cmd_table;
                char input[MAX_LINE_LEN];

                char read_buffer[MAX_LINE_LEN];
                memset(&input, '\0', sizeof(input));
                do {
                    memset(&read_buffer, '\0', sizeof(read_buffer));
                    read(sockfd, read_buffer, sizeof(read_buffer));
                    strcat(input, read_buffer);
                } while (read_buffer[strlen(read_buffer) - 1] != '\n');

                strtok(input, "\r\n");

                strcpy(raw_command, input);
                SplitString(input, cmd_table);
                if (cmd_table.command_length == 0) {
                    continue;
                }
                
                if (!strcmp(cmd_table.commands[0], "exit")) {
                    ClearPipefd(sockfd);
                    LogoutUser(activefds, sockfd);
                    continue;
                }

                CountdownPipefd(sockfd);
                
                if (!IsBuildInCmd(sockfd, cmd_table)) {
                    pid_t pid_table[MAX_COMMAND_NUM];
                    int pid_length = 0, index = 0;
                    while (index < cmd_table.command_length) {
                        int stdIOfd[3];
                        char command_path[MAX_COMMAND_LEN];
                        ParsedCmd cmd_list;
                        ParseCmd(cmd_table, index, cmd_list);

                        stdIOfd[0] = GetInputfd(sockfd, cmd_list);
                        stdIOfd[1] = GetOutputfd(sockfd, cmd_list);
                        stdIOfd[2] = GetErrorfd(sockfd, cmd_list);

                        GetCmdPath(cmd_list, command_path, sockfd);
                        ExecCmd(cmd_list, command_path, pid_table, pid_length, stdIOfd);
                        
                        ClosePipefd(sockfd, stdIOfd[0], cmd_list);
                    }
                }
                write(sockfd, "% ", strlen("% "));
            }
        }
    }
}

void ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
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

void BroadcastMsg(const char *msg) {
    for (int i = 1; i <= MAX_USER; ++i) {
        if (users[i].is_login) {
            write(users[i].sockfd, msg, strlen(msg));
        }
    }
}

int LoginUser(fd_set &activefds, int server_sockfd) {
    int client_sockfd;
    socklen_t addr_len;
    sockaddr_in client_addr;

    addr_len = sizeof(client_addr);
    client_sockfd = accept(server_sockfd, (sockaddr *) &client_addr, &addr_len);
    if (client_sockfd < 0) {
        cerr << "Error: accept error" << endl;
        exit(1);
    }

    FD_SET(client_sockfd, &activefds);

    WriteWLCMMsg(client_sockfd);

    // user init
    for (int i = 1; i <= MAX_USER; ++i) {
        if (users[i].is_login == false) {
            strcpy(users[i].name, "(no name)");
            char port[10];
            sprintf(port, "%d", ntohs(client_addr.sin_port));
            strcpy(users[i].ip_port, inet_ntoa(client_addr.sin_addr));
            strcat(users[i].ip_port, ":");
            strcat(users[i].ip_port, port);
            users[i].sockfd = client_sockfd;
            users[i].is_login = true;
            users[i].env.length = 0;
            strcpy(users[i].env.key[users[i].env.length], "PATH");
            strcpy(users[i].env.value[users[i].env.length], "bin:.");
            users[i].env.length++;

            LoginBroadcast(users[i]);
            break;
        }
    }

    return client_sockfd;
}

void LoginBroadcast(User user) {
    char msg[2000];
    sprintf(msg, "*** User '%s' entered from %s. ***\n", user.name, user.ip_port);
    BroadcastMsg(msg);
}

void LogoutUser(fd_set &activefds, int sockfd) {
    FD_CLR(sockfd, &activefds);
    int idx = GetUserIndex(sockfd);
    if (idx != -1) {
        users[idx].is_login = false;
        close(users[idx].sockfd);
        users[idx].sockfd = -1;
        LogoutBroadcast(users[idx]);
    }
}

void LogoutBroadcast(User user) {
    char msg[2000];
    sprintf(msg, "*** User '%s' left. ***\n", user.name);
    BroadcastMsg(msg);
}

int GetUserIndex(int sockfd) {
    for (int i = 1; i <= MAX_USER; ++i) {
        if (sockfd == users[i].sockfd && users[i].is_login) {
            return i;
        }
    }
    return -1;
}

void WriteWLCMMsg(int sockfd) {
    char message[] = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    write(sockfd, message, strlen(message));
}

bool IsBuildInCmd(int sockfd, CmdTable cmd_table) {

    if (!strcmp(cmd_table.commands[0], "yell")) {
        Yell(sockfd, cmd_table.commands[1]);
    } else if (!strcmp(cmd_table.commands[0], "name")) {
        Name(sockfd, cmd_table.commands[1]);
    } else if (!strcmp(cmd_table.commands[0], "who")) {
        Who(sockfd);
    } else if (!strcmp(cmd_table.commands[0], "tell")) {
        Tell(sockfd, atoi(cmd_table.commands[1]), cmd_table.commands[2]);
    } else if (!strcmp(cmd_table.commands[0], "printenv")) {
        Printenv(sockfd, cmd_table);
    } else if (!strcmp(cmd_table.commands[0], "setenv")) {
        Setenv(sockfd, cmd_table);
    } else {
        return false;
    }

    return true;
}

void Yell(int sockfd, const char *msg){
    char output[MAX_MSG_LEN];
    int idx = GetUserIndex(sockfd);
    if (idx != -1) {
        sprintf(output, "*** %s yelled ***: %s\n", users[idx].name, msg);
        BroadcastMsg(output);
    }
}

void Name(int sockfd, const char *name) {
    char output[1024];
    int idx = GetUserIndex(sockfd);
    if (idx != -1) {
        for (int i = 1; i <= MAX_USER; ++i) {
            if (!strcmp(users[i].name, name)) {
                sprintf(output, "*** User '%s' already exists. ***\n", users[i].name);
                write(sockfd, output, strlen(output));
                return;
            }
        } 
        strcpy(users[idx].name, name);
        sprintf(output, "*** User from %s is named '%s'. ***\n", users[idx].ip_port, users[idx].name);
        BroadcastMsg(output);
    }
}

void Printenv(int sockfd, CmdTable cmd_table) {
    int idx = GetUserIndex(sockfd);
    for (int i = 0; i < users[idx].env.length; ++i) {
        if (!strcmp(cmd_table.commands[1], users[idx].env.key[i])) {
            write(sockfd, users[idx].env.value[i], strlen(users[idx].env.value[i]));
            write(sockfd, "\n", strlen("\n"));
            return;
        }
    }
    char *env = getenv(cmd_table.commands[1]);
    write(sockfd, env, strlen(env));
    write(sockfd, "\n", strlen("\n"));
}

void Who(int sockfd) {
    char output[1024];
    strcpy(output, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
    write(sockfd, output, strlen(output));

    for (int i = 1; i <= MAX_USER; ++i) {
        if (users[i].is_login) {
            if (users[i].sockfd == sockfd) {
                sprintf(output, "%d\t%s\t%s\t%s\n", i, users[i].name, users[i].ip_port, "<-me");
            } else {
                sprintf(output, "%d\t%s\t%s\n", i, users[i].name, users[i].ip_port);
            }
            write(sockfd, output, strlen(output));
        }
    }
}

void Tell(int sockfd, int user_id, char *msg) {
    char output[MAX_MSG_LEN];
    int idx = GetUserIndex(sockfd);
    if (users[user_id].is_login) {
        sprintf(output, "*** %s told you ***: %s\n", users[idx].name, msg);
        write(users[user_id].sockfd, output, strlen(output));
    } else {
        sprintf(output, "*** Error: user #%d does not exist yet. ***\n", user_id);
        write(sockfd, output, strlen(output));
    }
}

void Setenv(int sockfd, CmdTable cmd_table) {
    int idx = GetUserIndex(sockfd);
    for (int i = 0; i < users[idx].env.length; ++i) {
        if (!strcmp(cmd_table.commands[1], users[idx].env.key[i])) {
            strcpy(users[idx].env.value[i], cmd_table.commands[2]);
            return;
        }
    }

    strcpy(users[idx].env.key[users[idx].env.length], cmd_table.commands[1]);
    strcpy(users[idx].env.value[users[idx].env.length], cmd_table.commands[2]);
    users[idx].env.length++;
}

void SplitString(char *input, CmdTable &cmd_table) {
    cmd_table.command_length = 0;

    char delim[] = " \n";
    char *pch = strtok(input, delim);

    bool isBuildIn = false;
    while (pch != NULL) {
        cmd_table.commands[cmd_table.command_length++] = pch;
        if ((!strcmp(cmd_table.commands[0], "yell") && cmd_table.command_length == 1) || 
            (!strcmp(cmd_table.commands[0], "name") && cmd_table.command_length == 1) ||
            (!strcmp(cmd_table.commands[0], "tell") && cmd_table.command_length == 2) ||
            (!strcmp(cmd_table.commands[0], "printenv") && cmd_table.command_length == 1) ||
            (!strcmp(cmd_table.commands[0], "setenv") && cmd_table.command_length == 2)) {
            isBuildIn = true;
            break;
        }
        pch = strtok(NULL, delim);
    }
    if (isBuildIn) {
        pch = strtok(NULL, "\n");
        cmd_table.commands[cmd_table.command_length++] = pch;
    }
}

void ParseCmd(CmdTable cmd_table, int &index, ParsedCmd &cmd_list) {
    cmd_list.op = NONE;
    cmd_list.in_pipe = false;
    cmd_list.count = 0;

    cmd_list.argc = 0;
    cmd_list.argv[cmd_list.argc++] = cmd_table.commands[index++];
    while (index < cmd_table.command_length) {
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
            if (cmd_table.commands[index][1] != '\0') {
                cmd_list.to_user = atoi(&cmd_table.commands[index][1]);
                cmd_list.op = USER_PIPE_OUT;

                if (cmd_table.command_length > index + 1 && cmd_table.commands[index + 1][0] == '<') {
                    index++;
                    continue;
                }
                break;
            }
            strcpy(cmd_list.filename, cmd_table.commands[index + 1]);
            index++;
            cmd_list.op = FILE_REDIRECTION;
            break;
        } else if (cmd_table.commands[index][0] == '<') {
            cmd_list.from_user = atoi(&cmd_table.commands[index][1]);
            cmd_list.in_pipe = true;
        } else {
            cmd_list.argv[cmd_list.argc++] = cmd_table.commands[index];
        }
        index++;
    }

    cmd_list.argv[cmd_list.argc] = NULL;
    index++;
}

void GetCmdPath(ParsedCmd cmd_list, char command_path[], int sockfd) {
    int idx = GetUserIndex(sockfd);
    if (idx != -1) {
        char myenv[MAX_COMMAND_LEN];
        strcpy(myenv, users[idx].env.value[0]);
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

}

void ExecCmd(ParsedCmd cmd_list, const char *command_path, pid_t pid_table[], int &pid_length, int stdIOfd[]) {
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
            execvp(command_path, cmd_list.argv);
        }

        exit(0);
    } else {
        pid_table[pid_length++] = pid;
        if (cmd_list.op == NONE || cmd_list.op == FILE_REDIRECTION) {
            if (cmd_list.op == NONE) {
                pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
            }
            if (cmd_list.op == FILE_REDIRECTION) {
                close(stdIOfd[1]);
            }
            if (cmd_list.op == USER_PIPE_NULL) {
                pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                close(stdIOfd[1]);
            }
        }
    }
}

void CountdownPipefd(int sockfd) {
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].sockfd == sockfd) {
            pipefd_table[i].count -= 1;
        }
    }
}

void CreatePipefd(int sockfd, ParsedCmd cmd_list, bool to_next) {
    pipe(pipefd_table[pipefd_length].IOfd);
    pipefd_table[pipefd_length].sockfd = sockfd;
    pipefd_table[pipefd_length].count = cmd_list.count;
    pipefd_table[pipefd_length].to_next = to_next;
    pipefd_length++;
}

int GetInputfd(int sockfd, ParsedCmd cmd_list) {
    if (cmd_list.in_pipe) {
        char msg[3000];
        int from_user = cmd_list.from_user, to_user = GetUserIndex(sockfd);

        if (users[from_user].is_login == false) {
            sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", from_user);
            write(sockfd, msg, strlen(msg));
            int fd = open("/dev/null", O_RDONLY);
            return fd;
        } else {
            int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
            if (user_pipe_idx == -1) {
                sprintf(msg, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", from_user, to_user);
                write(sockfd, msg, strlen(msg));
                int fd = open("/dev/null", O_RDONLY);
                return fd;
            } else {
                sprintf(msg, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", users[to_user].name, to_user, users[from_user].name, from_user, raw_command);
                BroadcastMsg(msg);
                close(user_pipes[user_pipe_idx].IOfd[1]);
                return user_pipes[user_pipe_idx].IOfd[0];
            }
        }
    }
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].count == 0 && pipefd_table[i].sockfd == sockfd) {
            close(pipefd_table[i].IOfd[1]);
            pipefd_table[i].IOfd[1] = -1;

            return pipefd_table[i].IOfd[0];
        }
    }

    return STDIN_FILENO;
}

int GetOutputfd(int sockfd, ParsedCmd &cmd_list) {
    if (cmd_list.op == NUMBER_PIPE || cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].sockfd == sockfd) {
                return pipefd_table[i].IOfd[1];
            }
        }
        CreatePipefd(sockfd, cmd_list, false);
        return pipefd_table[pipefd_length - 1].IOfd[1];
    } else if (cmd_list.op == ORDINARY_PIPE) {
        CreatePipefd(sockfd, cmd_list, true);
        return pipefd_table[pipefd_length - 1].IOfd[1];
    } else if (cmd_list.op == FILE_REDIRECTION) {
        int fd = open(cmd_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        return fd;
    } else if (cmd_list.op == USER_PIPE_OUT) {
        char msg[3000];
        int from_user = GetUserIndex(sockfd), to_user = cmd_list.to_user;
        if (to_user > MAX_USER || users[to_user].is_login == false) {
            sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", to_user);
            write(sockfd, msg, strlen(msg));
            cmd_list.op = USER_PIPE_NULL;
            int fd = open("/dev/null", O_WRONLY);
            return fd;
        } else {
            int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
            if (user_pipe_idx != -1) {
                sprintf(msg, "*** Error: the pipe #%d->#%d already exists. ***\n", from_user, to_user);
                write(sockfd, msg, strlen(msg));
                cmd_list.op = USER_PIPE_NULL;
                int fd = open("/dev/null", O_WRONLY);
                return fd;
            } else {
                sprintf(msg, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", users[from_user].name, from_user, raw_command, users[to_user].name, to_user);
                BroadcastMsg(msg);
                CreateUserPipe(from_user, to_user);
                return user_pipes[user_pipe_length - 1].IOfd[1];
            }
        }
    }

    return sockfd;
}

int GetErrorfd(int sockfd, ParsedCmd cmd_list) {
    if (cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_length; ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].sockfd == sockfd) {
                return pipefd_table[i].IOfd[1];
            }
        }
        CreatePipefd(sockfd, cmd_list, false);
        return pipefd_table[pipefd_length - 1].IOfd[1];
    }

    return sockfd;
}

void CreateUserPipe(int from_user, int to_user) {
    user_pipes[user_pipe_length].from_user = from_user;
    user_pipes[user_pipe_length].to_user = to_user;
    pipe(user_pipes[user_pipe_length].IOfd);
    user_pipe_length++;
}

int GetUserPipeIndex(int from_user, int to_user) {
    for (int i = 0; i < user_pipe_length; ++i) {
        if (user_pipes[i].from_user == from_user && user_pipes[i].to_user == to_user) {
            return i;
        }
    }
    return -1;
}

void ClosePipefd(int sockfd, int inputfd, ParsedCmd cmd_list) {
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].sockfd == sockfd) {
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

    if (cmd_list.in_pipe) {
        int to_user = GetUserIndex(sockfd), from_user = cmd_list.from_user;
        int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
        if (user_pipe_idx != -1) {
            close(user_pipes[user_pipe_idx].IOfd[0]);
            UserPipe temp = user_pipes[user_pipe_length - 1];
            user_pipes[user_pipe_length - 1] = user_pipes[user_pipe_idx];
            user_pipes[user_pipe_idx] = temp;
            user_pipe_length--;
        } else {
            close(inputfd);
        }
    }
}

void ClearPipefd(int sockfd) {
    for (int i = 0; i < pipefd_length; ++i) {
        if (pipefd_table[i].sockfd == sockfd) {
            close(pipefd_table[i].IOfd[0]);
            close(pipefd_table[i].IOfd[1]);

            pipefd_length -= 1;

            Pipefd temp = pipefd_table[pipefd_length];
            pipefd_table[pipefd_length] = pipefd_table[i];
            pipefd_table[i] = temp;
            i -= 1;
        }
    }

    int idx = GetUserIndex(sockfd);
    for (int i = 0; i < user_pipe_length; ++i) {
        if (user_pipes[i].from_user == idx || user_pipes[i].to_user == idx) {
            close(user_pipes[i].IOfd[0]);
            close(user_pipes[i].IOfd[1]);

            user_pipe_length -= 1;
            UserPipe temp = user_pipes[user_pipe_length];
            user_pipes[user_pipe_length] = user_pipes[i];
            user_pipes[i] = temp;
            i -= 1;

        }
    }
}