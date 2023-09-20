#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

#define MAX_COMMAND_LEN 1000

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
// pipefd[0] is read end, pipefd[1] is write end; count is for |num and !num; to_next = true: '|', to_next = false: "|num" or "!num".
struct Pipefd {
    int pipefd[2]; 
    int count;
    bool to_next;
};

void ChildHandler(int signo) {
    // -1 -> wait any child process, WHOHANG -> return immediately without wait, == 0 , use WHOHANG and no childPid return
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

void SplitString(string buffer, vector<string>& cmd_table) {
    stringstream ss(buffer);
    string tmp = "";
    while (getline(ss, tmp, ' ')) {
        cmd_table.push_back(tmp);
    }
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
    pipefd_table.push_back(new_pipe);
    pipe(pipefd_table.back().pipefd);
    pipefd_table.back().count = count;
    pipefd_table.back().to_next = to_next;

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

int GetOutputfd(vector<Pipefd> &pipefd_table, ParsedCommand cmd_list) {
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
    
    return STDOUT_FILENO;
}

int GetErrorfd(vector<Pipefd> &pipefd_table, ParsedCommand cmd_list) {
    if (cmd_list.op == NUMBER_PIPE_ERR) {
        // concat this output to next numbered pipe
        for (int i = 0; i < pipefd_table.size(); ++i) {
            if (pipefd_table[i].count == cmd_list.count && pipefd_table[i].to_next == false) return pipefd_table[i].pipefd[1];
        }
        return CreatePipefd(pipefd_table, cmd_list.count, false);
    }
    
    return STDERR_FILENO;
}

string GetPath(ParsedCommand cmd_list) {
    string cmd_path = "";
    if (cmd_list.argv[0] == "exit") {
        cmd_path = "exit";
    } else if (cmd_list.argv[0] == "printenv") {
        cmd_path = "printenv";
    } else if (cmd_list.argv[0] == "setenv") {
        cmd_path = "setenv";
    } else {
        char *env = getenv("PATH");
        stringstream ss(env);

        while (getline(ss, cmd_path, ':')) {
            cmd_path += "/" + cmd_list.argv[0];
            if (!access(cmd_path.c_str(), X_OK)) return cmd_path;
        }
        cmd_path = ""; // unknown command
    }
    return cmd_path;
}

void ExecCmd(ParsedCommand cmd_list, const string cmd_path, vector<pid_t>& pid_table, int fd[]) {
    if (cmd_path == "exit") {
        exit(0);
    } else if (cmd_path == "setenv") {
        setenv(cmd_list.argv[1].c_str(), cmd_list.argv[2].c_str(), 1);
    } else if (cmd_path == "printenv") {
        char *msg = getenv(cmd_list.argv[1].c_str());
        if (msg) cout << msg << endl;
    } else {
        // SIGCHLD let parent process trigger the handler if child process terminate or stop
        signal(SIGCHLD, ChildHandler);

        pid_t pid = fork();
        // repeat until seccessfully fork
        while (pid < 0) {
            int status;
            waitpid(-1, &status, 0);
            pid = fork();
        }
        if (pid == 0) {
            // dup2(old, new)
            if (fd[0] != STDIN_FILENO) dup2(fd[0], STDIN_FILENO);
            if (fd[1] != STDOUT_FILENO) dup2(fd[1], STDOUT_FILENO);
            if (fd[2] != STDERR_FILENO) dup2(fd[2], STDERR_FILENO);

            if (fd[0] != STDIN_FILENO) close(fd[0]);
            if (fd[1] != STDOUT_FILENO) close(fd[1]);
            if (fd[2] != STDERR_FILENO) close(fd[2]);

            if (cmd_path == "") {
                cerr << "Unknown command: [" << cmd_list.argv[0] << "]." << endl;
            } else {
                char* tmp[cmd_list.argv.size() + 1];
                for (int i = 0; i < cmd_list.argv.size(); ++i) tmp[i] = strdup(cmd_list.argv[i].c_str());
                tmp[cmd_list.argv.size()] = NULL;
                execvp(cmd_path.c_str(), tmp);
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

int main(int argc, char *argv[]) {
    string buffer;
    vector<Pipefd> pipefd_table;

    setenv("PATH", "bin:.", 1);

    // receive inputs
    while (true) {
        cout << "% ";
        if (!getline(cin, buffer)) {
            break;
        }
        
        vector<string> cmd_table;
        vector<pid_t> pid_table;
        int index = 0;

        CountdownPipefd(pipefd_table);

        SplitString(buffer, cmd_table);

        // execute parsed commands
        while (index < cmd_table.size()) {
            int fd[3];
            ParsedCommand cmd_list = ParseCmd(cmd_table, index);

            fd[0] = GetInputfd(pipefd_table);
            fd[1] = GetOutputfd(pipefd_table, cmd_list);
            fd[2] = GetErrorfd(pipefd_table, cmd_list);

            string cmd_path = GetPath(cmd_list);
            ExecCmd(cmd_list, cmd_path, pid_table, fd);
            ClosePipefd(pipefd_table);
        }
    }
    return 0;
}