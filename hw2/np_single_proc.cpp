#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <map>
#include <vector>

using namespace std;

#define MAX_LINE_LEN 15000
#define MAX_USER 30

#define NONE 1
#define ORDINARY_PIPE 2
#define NUMBER_PIPE 3
#define NUMBER_PIPE_ERR 4
#define FILE_REDIRECTION 5
#define USER_PIPE_OUT 6
#define USER_PIPE_NULL 7

struct User {
    string name;
    string ip_port;
    int sockfd;
    bool is_login;
    map<string, string> env;
};

struct ParsedCommand {
    int op;
    bool in_pipe;
    int from_user;
    int to_user;
    int count;
    vector<string> argv;
    string filename;
};

struct Pipefd {
    int pipefd[2];
    int count;
    int sockfd;
    bool to_next;
};

struct UserPipe {
    int pipefd[2];
    int from_user;
    int to_user;
};

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
        exit(EXIT_FAILURE);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        cerr << "Error: setsockopt failed" << endl;
        exit(EXIT_FAILURE);
    }
    if (bind(sockfd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Error: bind failed" << endl;
        exit(EXIT_FAILURE);
    }
    if (listen(sockfd, 0) < 0) {
        cerr << "Error: listen failed" << endl;
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

void BroadcastMsg(string msg, vector<User>& users) {
    for (int i = 0; i <= users.size(); ++i) {
        if (users[i].is_login) write(users[i].sockfd, msg.c_str(), msg.length());
    }
}

void LoginBroadcast(vector<User>& users, User user) {
    string msg = "*** User '" + user.name + "' entered from " + user.ip_port + ". ***\n";
    BroadcastMsg(msg, users);
}

void WriteWLCMMsg(int sockfd) {
    char message[] = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    write(sockfd, message, strlen(message));
}

int LoginUser(fd_set& activefds, int& server_sockfd, vector<User>& users) {
    sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_sockfd = accept(server_sockfd, (sockaddr *) &client_addr, &addr_len);
    if (client_sockfd < 0) {
        cerr << "Error: accept error" << endl;
        exit(EXIT_FAILURE);
    }
    FD_SET(client_sockfd, &activefds);

    WriteWLCMMsg(client_sockfd);

    // init new user
    User new_user;
    new_user.name = "(no name)";
    string port = to_string(ntohs(client_addr.sin_port));
    string ip_addr = inet_ntoa(client_addr.sin_addr);
    new_user.ip_port = ip_addr + ":" + port;
    new_user.sockfd = client_sockfd;
    new_user.is_login = true;
    new_user.env["PATH"] = "bin:.";
    users.push_back(new_user);
    LoginBroadcast(users, new_user);

    return client_sockfd;
}

int GetUserIndex(int sockfd, vector<User>& users) {
    for (int i = 0; i <= users.size(); ++i) {
        if (sockfd == users[i].sockfd && users[i].is_login) return i;
    }
    return -1;
}

void LogoutBroadcast(vector<User>& users, User user) {
    string msg = "*** User '" + user.name + "' left. ***\n";
    BroadcastMsg(msg, users);
}

void LogoutUser(int sockfd, fd_set &activefds, vector<User>& users) {
    FD_CLR(sockfd, &activefds);
    int idx = GetUserIndex(sockfd, users);
    if (idx != -1) {
        users[idx].is_login = false;
        close(users[idx].sockfd);
        users[idx].sockfd = -1;
        LogoutBroadcast(users, users[idx]);
    }
}

void Yell(int sockfd, string msg, vector<User>& users) {
    int idx = GetUserIndex(sockfd, users);
    if (idx != -1) {
        string output = "*** " + users[idx].name + " yelled ***: " + msg + "\n";
        BroadcastMsg(output, users);
    }
}

void Name(int sockfd, string name, vector<User>& users) {
    int idx = GetUserIndex(sockfd, users);
    if (idx != -1) {
        for (int i = 0; i <= users.size(); ++i) {
            if (users[i].name == name) {
                string output = "*** User '" + users[i].name + "' already exists. ***\n";
                write(sockfd, output.c_str(), output.length());
                return;
            }
        } 
        users[idx].name = name;
        string output = "*** User from " + users[idx].ip_port + " is named '" + users[idx].name + "'. ***\n";
        BroadcastMsg(output, users);
    }
}

void Who(int sockfd, vector<User>& users) {
    char output[1024];
    strcpy(output, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
    write(sockfd, output, strlen(output));

    for (int i = 0; i <= users.size(); ++i) {
        if (users[i].is_login) {
            if (users[i].sockfd == sockfd) {
                sprintf(output, "%d\t%s\t%s\t%s\n", i, users[i].name.c_str(), users[i].ip_port.c_str(), "<-me");
            } else {
                sprintf(output, "%d\t%s\t%s\n", i, users[i].name.c_str(), users[i].ip_port.c_str());
            }
            write(sockfd, output, strlen(output));
        }
    }
}

void Tell(int sockfd, int user_id, string msg, vector<User>& users) {
    int idx = GetUserIndex(sockfd, users);
    if (users[user_id].is_login) {
        string output = "*** " + users[idx].name + " told you ***: " + msg + "\n";
        write(users[user_id].sockfd, output.c_str(), output.length());
    } else {
        string output = "*** Error: user #" + to_string(user_id) + " does not exist yet. ***\n";
        write(sockfd, output.c_str(), output.length());
    }
}

void Printenv(int sockfd, vector<string>& cmd_table, vector<User>& users) {
    int idx = GetUserIndex(sockfd, users);
    for (auto it = users[idx].env.begin(); it != users[idx].env.end(); ++it) {
        if (cmd_table[1] == it->first) {
            write(sockfd, it->second.c_str(), it->second.length());
            write(sockfd, "\n", strlen("\n"));
            return;
        }
    }
    char *env = getenv(cmd_table[1].c_str());
    write(sockfd, env, strlen(env));
    write(sockfd, "\n", strlen("\n"));
}

void Setenv(int sockfd, vector<string>& cmd_table, vector<User>& users) {
    int idx = GetUserIndex(sockfd, users);
    users[idx].env[cmd_table[1]] = cmd_table[2];
}

bool IsBuildInCmd(int sockfd, vector<string>& cmd_table, vector<User>& users) {
    if (cmd_table[0] == "yell") {
        Yell(sockfd, cmd_table[1], users);
    } else if (cmd_table[0] == "name") {
        Name(sockfd, cmd_table[1], users);
    } else if (cmd_table[0] == "who") {
        Who(sockfd, users);
    } else if (cmd_table[0] == "tell") {
        Tell(sockfd, stoi(cmd_table[1]), cmd_table[2], users);
    } else if (cmd_table[0] == "printenv") {
        Printenv(sockfd, cmd_table, users);
    } else if (cmd_table[0] == "setenv") {
        Setenv(sockfd, cmd_table, users);
    }

    return false;
}

void SplitString(string buffer, vector<string>& cmd_table) {
    bool isBuildIn = false;
    stringstream ss(buffer);
    string tmp = "";
    while (getline(ss, tmp, ' ') || getline(ss, tmp, '\n')) {
        cmd_table.push_back(tmp);
        if (((cmd_table[0] == "yell") && cmd_table.size() == 1) || 
            ((cmd_table[0] == "name") && cmd_table.size() == 1) ||
            ((cmd_table[0] == "tell") && cmd_table.size() == 2) ||
            ((cmd_table[0] == "printenv") && cmd_table.size() == 1) ||
            ((cmd_table[0] == "setenv") && cmd_table.size() == 2)) {
            isBuildIn = true;
            break;
        }
    }
    if (isBuildIn) {
        getline(ss, tmp, '\n');
        cmd_table.push_back(tmp);
    }
}

ParsedCommand ParseCmd(vector<string> cmd_table, int &index) {
    ParsedCommand cmd_list;
    cmd_list.op = NONE;
    cmd_list.in_pipe = false;
    cmd_list.count = 0;
    cmd_list.argv.push_back(cmd_table[index++]);

    while (index < cmd_table.size()) {
        if (cmd_table[index][0] == '|') {
            if (cmd_table[index][1] != '\0') {
                cmd_list.op = NUMBER_PIPE;
                cmd_list.count = atoi(&cmd_table[index][1]);
            } else {
                cmd_list.op = ORDINARY_PIPE;
                cmd_list.count = 0;
            }
            break;
        } else if (cmd_table[index][0] == '!') {
            cmd_list.op = NUMBER_PIPE_ERR;
            cmd_list.count = atoi(&cmd_table[index][1]);
            break;
        } else if (cmd_table[index][0] == '>') {
            if (cmd_table[index][1] != '\0') {
                cmd_list.op = USER_PIPE_OUT;
                cmd_list.to_user = atoi(&cmd_table[index][1]);

                if (index + 1 < cmd_table.size() && cmd_table[index + 1][0] == '<') {
                    index++;
                    continue;
                }
            } else {
                cmd_list.op = FILE_REDIRECTION;
                cmd_list.filename = cmd_table[index + 1];
                index++;
            }
            break;
        } else if (cmd_table[index][0] == '<') {
            cmd_list.from_user = atoi(&cmd_table[index][1]);
            cmd_list.in_pipe = true;
        } else {
            cmd_list.argv.push_back(cmd_table[index]);
        }
        index++;
    }
    index++;

    return cmd_list;
}

string GetPath(ParsedCommand cmd_list, vector<User>& users, int sockfd) {
    string cmd_path = "";
    int idx = GetUserIndex(sockfd, users);
    if (idx != -1) {
        stringstream ss(users[idx].env["PATH"].c_str());

        while (getline(ss, cmd_path, ':')) {
            cmd_path += "/" + cmd_list.argv[0];
            if (!access(cmd_path.c_str(), X_OK)) return cmd_path;
        }
    }
    cmd_path = "";
    
    return cmd_path;
}

void ExecCmd(ParsedCommand cmd_list, string cmd_path, vector<pid_t> pid_table, int fd[]) {
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

        exit(EXIT_SUCCESS);
    } else {
        pid_table.push_back(pid);
        if (cmd_list.op == NONE || cmd_list.op == FILE_REDIRECTION || cmd_list.op == USER_PIPE_NULL) {
            for (int i = 0; i < pid_table.size(); ++i) {
                int status;
                waitpid(pid_table[i], &status, 0);
            }
            if (cmd_list.op == FILE_REDIRECTION || cmd_list.op == USER_PIPE_NULL) {
                close(fd[1]);
            }
        }
    }
}

void CountdownPipefd(int sockfd, vector<Pipefd> &pipefd_table) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        if (pipefd_table[i].sockfd == sockfd) pipefd_table[i].count--;
    }
}

int GetUserPipeIndex(vector<UserPipe>& user_pipes, int from_user, int to_user) {
    for (int i = 0; i < user_pipes.size(); ++i) {
        if (user_pipes[i].from_user == from_user && user_pipes[i].to_user == to_user) return i;
    }
    return -1;
}

int CreatePipefd(vector<Pipefd> &pipefd_table, ParsedCommand cmd_list, bool to_next, int sockfd) {
    Pipefd new_pipe;
    pipe(new_pipe.pipefd);
    new_pipe.sockfd = sockfd;
    new_pipe.count = cmd_list.count;
    new_pipe.to_next = to_next;
    pipefd_table.push_back(new_pipe);
    
    return pipefd_table.back().pipefd[1];
}

void CreateUserPipe(vector<UserPipe>& user_pipes, int from_user, int to_user) {
    UserPipe new_user_pipe;
    new_user_pipe.from_user = from_user;
    new_user_pipe.to_user = to_user;
    pipe(new_user_pipe.pipefd);
    user_pipes.push_back(new_user_pipe);
}

int GetInputfd(int sockfd, ParsedCommand cmd_list, vector<Pipefd> &pipefd_table, vector<UserPipe>& user_pipes, vector<User>& users, string buffer) {
    if (cmd_list.in_pipe) {
        int from_user = cmd_list.from_user, to_user = GetUserIndex(sockfd, users);

        if (users[from_user].is_login == false) {
            string msg = "*** Error: user #" + to_string(from_user) + " does not exist yet. ***\n";
            write(sockfd, msg.c_str(), msg.length());

            return open("/dev/null", O_RDONLY);
        } else {
            int user_pipe_idx = GetUserPipeIndex(user_pipes, from_user, to_user);

            if (user_pipe_idx == -1) {
                string msg = "*** Error: the pipe #" + to_string(from_user) + "->#" + to_string(to_user) + " does not exist yet. ***\n";
                write(sockfd, msg.c_str(), msg.length());

                return open("/dev/null", O_RDONLY);
            } else {
                string msg = "*** " + users[to_user].name + " (#" + to_string(to_user) + ") just received from " + users[from_user].name + " (#" + to_string(from_user) + ") by '" + buffer + "' ***\n";
                BroadcastMsg(msg, users);
                close(user_pipes[user_pipe_idx].pipefd[1]);

                return user_pipes[user_pipe_idx].pipefd[0];
            }
        }
    } else {
        for (int i = 0; i < pipefd_table.size(); ++i) {
            if (pipefd_table[i].count == 0 && pipefd_table[i].sockfd == sockfd) {
                close(pipefd_table[i].pipefd[1]);
                pipefd_table[i].pipefd[1] = -1;

                return pipefd_table[i].pipefd[0];
            }
        }
    }

    return STDIN_FILENO;
}

int GetOutputfd(int sockfd, ParsedCommand &cmd_list, vector<Pipefd> &pipefd_table, vector<UserPipe>& user_pipes, vector<User>& users, string buffer) {
    if (cmd_list.op == NUMBER_PIPE || cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_table.size(); ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].sockfd == sockfd) return pipefd_table[i].pipefd[1];
        }
        return CreatePipefd(pipefd_table, cmd_list, false, sockfd);
    } else if (cmd_list.op == ORDINARY_PIPE) {
        return CreatePipefd(pipefd_table, cmd_list, true, sockfd);
    } else if (cmd_list.op == FILE_REDIRECTION) {
        return open(cmd_list.filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    } else if (cmd_list.op == USER_PIPE_OUT) {
        int from_user = GetUserIndex(sockfd, users), to_user = cmd_list.to_user;
        if (to_user > MAX_USER || users[to_user].is_login == false) {
            cmd_list.op = USER_PIPE_NULL;
            string msg = "*** Error: user #" + to_string(to_user) + " does not exist yet. ***\n";
            write(sockfd, msg.c_str(), msg.length());
            return open("/dev/null", O_WRONLY);;
        } else {
            int user_pipe_idx = GetUserPipeIndex(user_pipes, from_user, to_user);

            if (user_pipe_idx != -1) {
                cmd_list.op = USER_PIPE_NULL;
                string msg = "*** Error: the pipe #" + to_string(from_user) + "->#" + to_string(to_user) + " already exists. ***\n";
                write(sockfd, msg.c_str(), msg.length());
                return open("/dev/null", O_WRONLY);;
            } else {
                string msg = "*** " + users[from_user].name + " (#" + to_string(from_user) + ") just piped '" + buffer + "' to " + users[to_user].name + " (#" + to_string(to_user) + ") ***\n";
                BroadcastMsg(msg, users);
                CreateUserPipe(user_pipes, from_user, to_user);
                return user_pipes.back().pipefd[1];
            }
        }
    }

    return sockfd;
}

int GetErrorfd(int sockfd, ParsedCommand cmd_list, vector<Pipefd>& pipefd_table) {
    if (cmd_list.op == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipefd_table.size(); ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].sockfd == sockfd) return pipefd_table[i].pipefd[1];
        }
        return CreatePipefd(pipefd_table, cmd_list, false, sockfd);
    }

    return sockfd;
}

void ClosePipefd(int sockfd, ParsedCommand cmd_list, vector<Pipefd>& pipefd_table, vector<UserPipe>& user_pipes, vector<User>& users, int inputfd) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        if (pipefd_table[i].sockfd == sockfd && pipefd_table[i].count <= 0 && pipefd_table[i].to_next == false) {
            close(pipefd_table[i].pipefd[0]);
            pipefd_table[i].pipefd[0] = -1;
            
            pipefd_table.erase(pipefd_table.begin() + i);
            i--;
        } else if (pipefd_table[i].to_next == true) {
            pipefd_table[i].to_next = false;
        }
    }

    if (cmd_list.in_pipe) {
        int to_user = GetUserIndex(sockfd, users), from_user = cmd_list.from_user;
        int user_pipe_idx = GetUserPipeIndex(user_pipes, from_user, to_user);
        if (user_pipe_idx != -1) {
            close(user_pipes[user_pipe_idx].pipefd[0]);
            user_pipes.erase(user_pipes.begin() + user_pipe_idx);
        } else {
            close(inputfd);
        }
    }
}

void ClearPipefd(int sockfd, vector<Pipefd>& pipefd_table, vector<UserPipe>& user_pipes, vector<User>& users) {
    for (int i = 0; i < pipefd_table.size(); ++i) {
        if (pipefd_table[i].sockfd == sockfd) {
            close(pipefd_table[i].pipefd[0]);
            close(pipefd_table[i].pipefd[1]);

            pipefd_table.erase(pipefd_table.begin() + i);
            i--;
        }
    }

    int idx = GetUserIndex(sockfd, users);
    for (int i = 0; i < user_pipes.size(); ++i) {
        if (user_pipes[i].from_user == idx || user_pipes[i].to_user == idx) {
            close(user_pipes[i].pipefd[0]);
            close(user_pipes[i].pipefd[1]);

            user_pipes.erase(user_pipes.begin() + i);
            i--;
        }
    }
}

int main(int argc, char* argv[]) {
    vector<User> users;
    vector<UserPipe> user_pipes;
    vector<Pipefd> pipefd_table;
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_single_proc [port]" << endl;
        exit(1);
    }
    
    int server_sockfd = TCPconnect(atoi(argv[1]));

    /* 
    FD_ZERO(fd_set *): clear the fd_set
    FD_SET(int, fd_set*): add fd to a set
    FD_CLR(int, fd_set*): delete fd from a set
    FD_ISSET(int, fd_set*): check if a fd is accessible
    int select(int maxfdp, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout):
    non-blocking monitoring the fds
    */
    fd_set readfds, activefds;
    int fds_max_num = getdtablesize(); // get fdtable size
    FD_ZERO(&activefds);
    FD_SET(server_sockfd, &activefds);

    while (true) {
        memcpy(&readfds, &activefds, sizeof(readfds));

        if (select(fds_max_num, &readfds, NULL, NULL, NULL) < 0) {
            cerr << "Error: select error" << endl;
            continue;
        }
        if (FD_ISSET(server_sockfd, &readfds)) {
            int client_sockfd = LoginUser(activefds, server_sockfd, users);
            write(client_sockfd, "% ", strlen("% "));
        }
        for (int sockfd = 0; sockfd < fds_max_num; ++sockfd) {
            if (server_sockfd != sockfd && FD_ISSET(sockfd, &readfds)) {
                char read_buffer[MAX_LINE_LEN];
                char input[MAX_LINE_LEN];
                
                // write(sockfd, "% ", strlen("% "));
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
                if (cmd_table[0] == "exit") {
                    ClearPipefd(sockfd, pipefd_table, user_pipes, users);
                    LogoutUser(sockfd, activefds, users);
                    continue;
                }

                CountdownPipefd(sockfd, pipefd_table);
                
                if (!IsBuildInCmd(sockfd, cmd_table, users)) {
                    while (index < cmd_table.size()) {
                        int fd[3];
                        ParsedCommand cmd_list = ParseCmd(cmd_table, index);

                        fd[0] = GetInputfd(sockfd, cmd_list, pipefd_table, user_pipes, users, buffer);
                        fd[1] = GetOutputfd(sockfd, cmd_list, pipefd_table, user_pipes, users, buffer);
                        fd[2] = GetErrorfd(sockfd, cmd_list, pipefd_table);

                        string cmd_path = GetPath(cmd_list, users, sockfd);
                        ExecCmd(cmd_list, cmd_path, pid_table, fd);
                        ClosePipefd(sockfd, cmd_list, pipefd_table, user_pipes, users, fd[0]);
                    }
                }
            }
        }
    }
}