#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <vector>

using namespace std;

#define MAX_LINE_LEN 15000

#define NONE 1
#define ORDINARY_PIPE 2
#define NUMBER_PIPE 3
#define NUMBER_PIPE_ERR 4
#define FILE_REDIRECTION 5

struct ParsedCommand {
    int op;
    int count;
    vector<string> argv;
    string filename;
};

struct Pipefd {
    int pipefd[2];
    int count;
    bool to_next;
};

void ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

int TCPconnect(uint16_t port) {
    int sockfd, optval = 1;
    sockaddr_in serv_addr;

    // AF_INET: IPv4, SOCK_STREAM: TCP
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        cerr << "Error: socket failed" << endl;
        exit(-1);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        cerr << "Error: setsockopt failed" << endl;
        exit(-1);
    }
    if (bind(sockfd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Error: bind failed" << endl;
        exit(-1);
    }
    if (listen(sockfd, 0) < 0) {
        cerr << "Error: listen failed" << endl;
        exit(-1);
    }

    return sockfd;
}

void SplitString(string buffer, vector<string>& cmd_table) {
    stringstream ss(buffer);
    string tmp = "";
    while (getline(ss, tmp, ' ') || getline(ss, tmp, '\n')) {
        cmd_table.push_back(tmp);
    }
}

bool IsBuildInCmd(vector<string> cmd_table, map<string, string>& env, int sockfd) {
    if (cmd_table[0] == "setenv") {
        env[cmd_table[1]] = cmd_table[2];
        return true;
    } else if (cmd_table[0] == "printenv") {
        for (auto it = env.begin(); it != env.end(); ++it) {
            if (it->first == cmd_table[1]) {
                write(sockfd, it->second.c_str(), it->second.length());
                write(sockfd, "\n", strlen("\n"));
                return true;
            }
        }
        char *myenv = getenv(cmd_table[1].c_str());
        if (myenv) write(sockfd, myenv, strlen(myenv));
        write(sockfd, "\n", strlen("\n"));
        return true;
    }

    return false;
}

ParsedCommand ParseCmd(vector<string> cmd_table, int& index) {
    ParsedCommand cmd_list;
    cmd_list.op = NONE;
    cmd_list.count = 0;
    cmd_list.argv.push_back(cmd_table[index++]);
    
    while (index < cmd_table.size()) {
        if (cmd_table[index][0] == '|') {
            if (cmd_table[index][1] != '\0') {
                cmd_list.op = NUMBER_PIPE;
                cmd_list.count = stoi(&cmd_table[index][1]);
            } else {
                cmd_list.op = ORDINARY_PIPE;
                cmd_list.count = 0;
            }
            break;
        } else if (cmd_table[index][0] == '!') {
            cmd_list.op = NUMBER_PIPE_ERR;
            cmd_list.count = stoi(&cmd_table[index][1]);
            break;
        } else if (cmd_table[index][0] == '>') {
            cmd_list.op = FILE_REDIRECTION;
            cmd_list.filename = cmd_table[++index];
            break;
        } else {
            cmd_list.argv.push_back(cmd_table[index++]);
        }
    }
    index++;
    
    return cmd_list;
}

void CountdownPipefd(vector<Pipefd> &pipefd_table) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        pipefd_table[i].count--;
    }
}


int CreatePipefd(vector<Pipefd> &pipefd_table, int count, bool to_next) {
    Pipefd new_pipe;
    pipe(new_pipe.pipefd);
    new_pipe.count = count;
    new_pipe.to_next = to_next;
    pipefd_table.push_back(new_pipe);

    return pipefd_table.back().pipefd[1];
}

int GetInputfd(vector<Pipefd> &pipefd_table) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        // close the write end if find any numbered pipe would be input
        if (pipefd_table[i].count == 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].pipefd[1]);
            pipefd_table[i].pipefd[1] = -1;

            return pipefd_table[i].pipefd[0];
        }
    }

    return STDIN_FILENO;
}

int GetOutputfd(vector<Pipefd> &pipefd_table, ParsedCommand cmd_list, int sockfd) {
    switch (cmd_list.op) {
        case NUMBER_PIPE:
        case NUMBER_PIPE_ERR:
            // concat this output to next numbered pipe
            for (int i = 0; i < pipefd_table.size(); ++i) {
                if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].to_next == false) return pipefd_table[i].pipefd[1];
            }
            return CreatePipefd(pipefd_table, cmd_list.count, false);
        case ORDINARY_PIPE:
            return CreatePipefd(pipefd_table, cmd_list.count, true);
        case FILE_REDIRECTION:
            return open(cmd_list.filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    }
    
    return sockfd;
}

int GetErrorfd(vector<Pipefd> &pipefd_table, ParsedCommand cmd_list, int sockfd) {
    if (cmd_list.op == NUMBER_PIPE_ERR) {
        // concat this output to next numbered pipe
        for (int i = 0; i < pipefd_table.size(); ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].to_next == false) return pipefd_table[i].pipefd[1];
        }
        return CreatePipefd(pipefd_table, cmd_list.count, false);
    }
    
    return sockfd;
}

string GetPath(ParsedCommand cmd_list, map<string, string> env) {
    string cmd_path = "";
    stringstream ss(env["PATH"].c_str());

    while (getline(ss, cmd_path, ':')) {
        cmd_path += "/" + cmd_list.argv[0];
        if (!access(cmd_path.c_str(), X_OK)) return cmd_path;
    }
    cmd_path = "";
    
    return cmd_path;
}

void ExecCmd(ParsedCommand cmd_list, const string cmd_path, vector<pid_t>& pid_table, int fd[]) {
    if (cmd_path == "setenv") {
        setenv(cmd_list.argv[1].c_str(), cmd_list.argv[2].c_str(), 1);
    } else if (cmd_path == "printenv") {
        char *msg = getenv(cmd_list.argv[1].c_str());
        if (msg) write(fd[1], msg, strlen(msg));
    } else {
        signal(SIGCHLD, ChildHandler);
        pid_t pid = fork();
        while (pid < 0) {
            int status;
            waitpid(-1, &status, 0);
            pid = fork();
        }
        if (pid == 0) {
            dup2(fd[0], STDIN_FILENO);
            dup2(fd[1], STDOUT_FILENO);
            dup2(fd[2], STDERR_FILENO);

            if (fd[0] != STDIN_FILENO) close(fd[0]);
            if (fd[1] != STDOUT_FILENO) close(fd[1]);
            if (fd[2] != STDERR_FILENO) close(fd[2]);

            if (cmd_path == "") {
                cerr << "Unknown command: [" << cmd_list.argv[0] << "]." << endl;
            } else {
                char* tmp[cmd_list.argv.size() + 1];
                for (int i = 0; i < cmd_list.argv.size(); ++i) tmp[i] = strdup(cmd_list.argv[i].c_str());
                tmp[cmd_list.argv.size()] = NULL;
                while (execvp(cmd_path.c_str(), tmp) < 0) cerr << "error exec" << endl;
                for (int i = 0; tmp[i] != NULL; ++i) free(tmp[i]);
            }

            exit(0);
        } else {
            pid_table.push_back(pid);
            if (cmd_list.op == NONE || cmd_list.op == FILE_REDIRECTION) {
                for (int i = 0; i < pid_table.size(); ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                if (cmd_list.op == FILE_REDIRECTION) {
                    close(fd[1]);
                }
            }
        }
    }
}

void ClosePipefd(vector<Pipefd> &pipefd_table) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        if (pipefd_table[i].count <= 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].pipefd[0]);
            pipefd_table[i].pipefd[0] = -1;

            pipefd_table.erase(pipefd_table.begin() + i);
            i--;
        } else if (pipefd_table[i].to_next == true) {
            pipefd_table[i].to_next = false;
        }
    }
}

void ExecSocket(int sockfd, map<string, string> &env) {
    vector<Pipefd> pipefd_table;

    while (true) {
        char read_buffer[MAX_LINE_LEN];
        char input[MAX_LINE_LEN];

        write(sockfd, "% ", strlen("% "));
        memset(&input, '\0', sizeof(input));
        do {
            memset(&read_buffer, '\0', sizeof(read_buffer));
            read(sockfd, read_buffer, sizeof(read_buffer));
            strcat(input, read_buffer);
        } while (read_buffer[strlen(read_buffer) - 1] != '\n');
        strtok(input, "\r\n");

        string buffer = input;
        vector<string> cmd_table;
        vector<pid_t> pid_table;
        int index = 0;

        SplitString(buffer, cmd_table);
        if (cmd_table.size() == 0) continue;
        if (cmd_table[0] == "exit") return;

        CountdownPipefd(pipefd_table);

        if (!IsBuildInCmd(cmd_table, env, sockfd)) {
            while (index < cmd_table.size()) {
                int fd[3];
                ParsedCommand cmd_list = ParseCmd(cmd_table, index);

                fd[0] = GetInputfd(pipefd_table);
                fd[1] = GetOutputfd(pipefd_table, cmd_list, sockfd);
                fd[2] = GetErrorfd(pipefd_table, cmd_list, sockfd);

                string cmd_path = GetPath(cmd_list, env);
                ExecCmd(cmd_list, cmd_path, pid_table, fd);
                ClosePipefd(pipefd_table);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_simple [port]" << endl;
        exit(1);
    }

    int server_sockfd = TCPconnect(atoi(argv[1]));
    sockaddr_in client_addr;

    while (true) {
        socklen_t addr_len = sizeof(client_addr);
        int client_sockfd = accept(server_sockfd, (sockaddr *) &client_addr, &addr_len);
        if (client_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }

        map<string, string> env;
        env["PATH"] =  "bin:.";

        ExecSocket(client_sockfd, env);
        close(client_sockfd);
    }
    close(server_sockfd);
    return 0;
}