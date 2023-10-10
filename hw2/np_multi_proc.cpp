#include <sstream>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <queue>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/shm.h>

using namespace std;

#define MAX_USER 30
#define MAX_MSG_LEN 1024
#define MAX_PATH_SIZE 30
#define SHM_KEY 1111
#define SHM_MSG_KEY 2222
#define SHM_FIFO_KEY 3333
#define FIFO_PATH "user_pipe/"

struct PipeFd {
    int in_fd;
    int out_fd;
    int count;
};
struct ClientInfo {
    int id;
    int pid;
    int valid;
    char user_name[20];
    char user_ip[INET_ADDRSTRLEN];
    int port;
};
struct FIFO {
    int in_fd;
    int out_fd;
    char name[MAX_PATH_SIZE];
    bool is_used;
};
struct FIFOInfo {
    FIFO fifo[MAX_USER][MAX_USER];
};
// global server variable
int g_shmid_cli;
int g_shmid_msg;
int g_shmid_fifo;
// client
int cur_id;
// create user variable
vector<ClientInfo> client_table;
vector<string> path_vector;
string terminal_input = "";
queue<string> cmds;
vector<pid_t> pid_vector;
vector<PipeFd> pipe_vector;

ClientInfo* GetCliSHM(int g_shmid_cli) {
    ClientInfo* shm = (ClientInfo*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (ClientInfo*)0) {
		cerr << "Error: shmat() failed" << endl;
		exit(EXIT_FAILURE);
	}
    return shm;
}

char* GetMsgSHM(int g_shmid_msg) {
    char* shm = (char*)shmat(g_shmid_msg, NULL, 0);
	if (shm < (char*)0) {
		cerr << "Error: shmat() failed" << endl;
		exit(EXIT_FAILURE);
	}
    return shm;
}

FIFOInfo* GetFIFOSHM(int g_shmid_fifo) {
    FIFOInfo* shm = (FIFOInfo*)shmat(g_shmid_fifo, NULL, 0);
	if (shm < (FIFOInfo*)0) {
		cerr << "Error: shmat() failed" << endl;
		exit(EXIT_FAILURE);
	}
    return shm;
}

ClientInfo* GetClientByID(int id, ClientInfo* shm_cli) {
    shm_cli = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		if ((shm_cli[i].id == id) && (shm_cli[i].valid == 1)) {
            ClientInfo* info = &shm_cli[i];
			return info;
		}
	}
    return NULL;
}

void Broadcast(string action, string msg, int cur_id, int target_id) {
    ClientInfo* shm_cli;
    ClientInfo* target_cli = (target_id != -1 ? GetClientByID(target_id, shm_cli) : NULL);
    ClientInfo* cur_cli = GetClientByID(cur_id, shm_cli);
    if (cur_cli == NULL) {
        fprintf(stderr, "cannot find client id: %d\n", cur_id);
        return;
    }

    char send_msg[MAX_MSG_LEN];
    if (action == "login") {
        sprintf(send_msg, "*** User '(no name)' entered from %s:%d. ***\n", cur_cli->user_ip, cur_cli->port);
    } else if (action == "logout") {
        sprintf(send_msg, "*** User '%s' left. ***\n", cur_cli->user_name);
    } else if (action == "name") {
        sprintf(send_msg, "*** User from %s:%d is named '%s'. ***\n", cur_cli->user_ip, cur_cli->port, cur_cli->user_name);
    } else if (action == "yell") {
        sprintf(send_msg, "*** %s yelled ***: %s\n", cur_cli->user_name, msg.c_str());
    } else if (action == "send") {
        sprintf(send_msg, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", cur_cli->user_name, cur_cli->id, msg.c_str(), target_cli->user_name, target_cli->id);
    } else if (action == "recv") {
        sprintf(send_msg, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", cur_cli->user_name, cur_cli->id, target_cli->user_name, target_cli->id, msg.c_str());
    } else if (action == "tell") {
        sprintf(send_msg, "*** %s told you ***: %s\n", cur_cli->user_name, msg.c_str());
    }

    char* shm_msg = GetMsgSHM(g_shmid_msg);
    sprintf(shm_msg, "%s", send_msg);
    shmdt(shm_msg);
    usleep(500);

    // shot down process
    shm_cli = GetCliSHM(g_shmid_cli);
    if (action != "tell") {
        for (int i = 0; i < MAX_USER; ++i) {
            if (shm_cli[i].valid == 1) kill(shm_cli[i].pid, SIGUSR1);
        }
    } else {
        kill(shm_cli[target_id-1].pid, SIGUSR1);
    }
	shmdt(shm_cli);
}

void ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

void ServerSigHandler(int sig) {
	if (sig == SIGCHLD) {
		while (waitpid (-1, NULL, WNOHANG) > 0);
	} else if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM) {
        shmctl(g_shmid_cli, IPC_RMID, NULL);
        shmctl(g_shmid_msg, IPC_RMID, NULL);
        shmctl(g_shmid_fifo, IPC_RMID, NULL);
		exit(EXIT_SUCCESS);
	}
	signal(sig, ServerSigHandler);
}

void ResetCliFIFOSHM() {
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
    for (size_t i = 0; i < MAX_USER; ++i) {
        if (shm_fifo->fifo[i][cur_id-1].out_fd != -1) {
            // read out message in the unused fifo
            char buf[1024];
            while (read(shm_fifo->fifo[i][cur_id-1].out_fd, &buf, sizeof(buf)) > 0) {}
            shm_fifo->fifo[i][cur_id-1].out_fd = -1;
            shm_fifo->fifo[i][cur_id-1].in_fd = -1;
            shm_fifo->fifo[i][cur_id-1].is_used = false;
            unlink(shm_fifo->fifo[i][cur_id-1].name);
            memset(shm_fifo->fifo[i][cur_id-1].name, 0, sizeof(shm_fifo->fifo[cur_id-1][i].name));
        }
    }
    shmdt(shm_fifo);
}

void SigHandler(int sig) {
	if (sig == SIGUSR1) { // receive messages from others
		char* msg = GetMsgSHM(g_shmid_msg);
        if (write(STDOUT_FILENO, msg, strlen(msg)) < 0) {
            cerr << "Error: broadcast_catch() failed" << endl;
        }
        shmdt(msg);
	} else if (sig == SIGUSR2) { // open FIFOs to read
        FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
		for (int i = 0; i < MAX_USER; ++i) {
			if (shm_fifo->fifo[i][cur_id-1].out_fd == -1 && shm_fifo->fifo[i][cur_id-1].name[0] != 0) {
                shm_fifo->fifo[i][cur_id-1].out_fd = open(shm_fifo->fifo[i][cur_id-1].name, O_RDONLY);
            }
		}
        shmdt(shm_fifo);
	} else if (sig == SIGINT || sig == SIGQUIT || sig == SIGTERM) { // clean FIFOs
        Broadcast("logout", "", cur_id, -1);
        ResetCliFIFOSHM();
	}
	signal(sig, SigHandler);
}

vector<string> Split(string s, char delimiter) {
    vector<string> res;
    stringstream ss(s);
    string tmp = "";
    while (getline(ss, tmp, delimiter)) {
        res.push_back(tmp);
    }
    return res;
}

void SetEnv(string var, string val) {
    setenv(var.c_str(), val.c_str(), 1);
    if (var == "PATH") {
        path_vector.clear();
        vector<string> res = Split(val, ':');
        for(vector<string>::iterator it = res.begin(); it != res.end(); ++it) {
            path_vector.push_back(*it);
        }
    }
}

void CreatePipe(vector<PipeFd> &pipe_vector, int pipe_num, int &in_fd) {
    // check if pipe to same pipe
    // has same pipe => reuse old pipe (multiple write, one read)
    // no same pipe => create new pipe (one write, one read)
    bool has_same_pipe = false;

    for (auto it = pipe_vector.begin(); it != pipe_vector.end(); ++it) {
        if (it->count == pipe_num) {
            has_same_pipe = true;
            in_fd = it->in_fd;
        }
    }
    if (has_same_pipe) return;

    // create pipe
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        cerr << "pipe error" << endl;
        exit(EXIT_FAILURE);
    }
    PipeFd new_pipefd;
    new_pipefd.in_fd = pipe_fd[1];  // write fd
    new_pipefd.out_fd = pipe_fd[0];  // read fd
    new_pipefd.count = pipe_num;
    pipe_vector.push_back(new_pipefd);
    in_fd = pipe_fd[1];
}

void CreateUserPipe(int cur_id, int target_id, int &out_fd) {
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);

    if (mkfifo(fifopath, 0666 | S_IFIFO) < 0) {
        cerr << "mkfifo error" << endl;
        exit(EXIT_FAILURE);
    }
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    ClientInfo* target_cli = GetClientByID(target_id, shm_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
    strncpy(shm_fifo->fifo[cur_id-1][target_id-1].name, fifopath, MAX_PATH_SIZE);

    // signal target client to open fifo and read
    kill(target_cli->pid, SIGUSR2);
    shmdt(shm_cli);
    shmdt(shm_fifo);
}

void GetUserPipeFd(int source_id, int cur_id, int& in_fd) {
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, source_id, cur_id);
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    shm_fifo->fifo[source_id-1][cur_id-1].in_fd = -1;
    in_fd = shm_fifo->fifo[source_id-1][cur_id-1].out_fd;
    shm_fifo->fifo[source_id-1][cur_id-1].is_used = true;

    shmdt(shm_cli);
    shmdt(shm_fifo);
}

bool GetPipeFd(int& in_fd) {
    for (auto it = pipe_vector.begin(); it != pipe_vector.end(); ++it) {
        if (it->count == 0) {
            close(it->in_fd);
            in_fd = it->out_fd;
            return true;
        }
    }
    return false;
}

void PrintEnv(string var) {
    char* val = getenv(var.c_str());
    if (val != NULL) {
        cout << val << endl;
    }
}

void Who(void) {
    ClientInfo* shm_cli;
    ClientInfo* cur_cli = GetClientByID(cur_id, shm_cli);

    cout << "<ID>\t" << "<nickname>\t" << "<IP:port>\t" << "<indicate me>" << endl;
    for (size_t id = 0; id < MAX_USER; ++id) {
        ClientInfo* cli_info = GetClientByID(id + 1, shm_cli);
        if (cli_info != NULL) {
            cout << cli_info->id << "\t" << cli_info->user_name << "\t" << cli_info->user_ip << ":" << cli_info->port;
            if (id + 1 == cur_cli->id) {
                cout << "\t" << "<-me" << endl;
            } else {
                cout << "\t" << endl;
            }
        }
    }
    shmdt(shm_cli);
}

void Yell(string msg) {
    Broadcast("yell", msg, cur_id, -1);
}

void Tell(int target_id, string msg) {
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    if(shm_cli[target_id-1].valid != 0) {
        Broadcast("tell", msg, cur_id, target_id);
    } else {
        fprintf(stderr, "*** Error: user #%d does not exist yet. ***\n", target_id);
    }
    shmdt(shm_cli);
}

void Name(string name) {
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    for (size_t i = 0; i < MAX_USER; ++i) {
        if (!strcmp(shm_cli[i].user_name, name.c_str())) {
            fprintf(stderr, "*** User '%s' already exists. ***\n", name.c_str());
            return;
        }
    }
    strcpy(shm_cli[cur_id-1].user_name, name.c_str());
    shmdt(shm_cli);
    Broadcast("name", "", cur_id, -1);
}

void SetUserPipeOut(int target_id, int& out_fd) {
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    out_fd = open(fifopath, O_WRONLY);
    shm_fifo->fifo[cur_id-1][target_id-1].in_fd = out_fd;

    shmdt(shm_cli);
    shmdt(shm_fifo);
}

int ExecCmd(vector<string> &arguments, int in_fd, int out_fd, int err_fd, bool line_ends, bool is_using_pipe, int target_id) {
    char *args[arguments.size() + 1];
    for (size_t i = 0; i < arguments.size(); ++i) {
        args[i] = new char[arguments[i].size() + 1];
        strcpy(args[i], arguments[i].c_str());
    }

    // built-in
    string prog(args[0]);
    if (prog == "printenv") {
        PrintEnv(args[1]);
    } else if (prog == "setenv") {
        SetEnv(args[1], args[2]);
    } else if (prog == "who") {
        Who();
    } else if (prog == "yell") {
        string msg = "";
        for (size_t i = 1; i < arguments.size(); ++i) {
            msg += string(args[i]) + " ";
        }
        msg.pop_back();

        Yell(msg);
    } else if (prog == "tell") {
        string msg = "";
        for (size_t i = 2; i < arguments.size(); ++i) {
            msg += string(args[i]) + " ";
        }
        msg.pop_back();

        Tell(stoi(args[1]), msg);
    } else if (prog == "name") {
        Name(string(args[1]));
    } else if (prog == "exit") {
        return -1;
    } else { // not built-in
        signal(SIGCHLD, ChildHandler);
        pid_t child_pid = fork();
        while (child_pid < 0) {
            usleep(1000);
            child_pid = fork();
        }
        if (child_pid == 0) { // child process
            // current client open FIFO and record write fd
            if (target_id > 0) SetUserPipeOut(target_id, out_fd);

            // pipe ops
            if (in_fd != STDIN_FILENO) dup2(in_fd, STDIN_FILENO);
            if (out_fd != STDOUT_FILENO) dup2(out_fd, STDOUT_FILENO);
            if (err_fd != STDERR_FILENO) dup2(err_fd, STDERR_FILENO);
            if (in_fd != STDIN_FILENO) close(in_fd);
            if (out_fd != STDOUT_FILENO) close(out_fd);
            if (err_fd != STDERR_FILENO) close(err_fd);

            bool is_executable = false;
            for (auto it = path_vector.begin(); it != path_vector.end(); ++it) {
                string path = *it + "/" + prog;
                if (!access(path.c_str(), X_OK)) is_executable = true;
            }

            if (!is_executable) {
                cerr << "Unknown command: [" << args[0] << "]." << endl;
            } else {
                args[arguments.size()] = NULL;
                if(execvp(args[0], args) < 0) cerr << "execl error" << endl;
            }

            exit(EXIT_SUCCESS);
        } else { // parent process
            if (line_ends && !is_using_pipe) {
                int status;
                waitpid(child_pid, &status, 0);
            }
        }
    }
    return 0;
}

void CountdownPipefd() {
    for (auto it = pipe_vector.begin(); it != pipe_vector.end(); ++it) {
        it->count--;
    }
}

void ErasePipefd() {
    for (auto it = pipe_vector.begin(); it != pipe_vector.end();) {
        if (it->count == 0) {
            close(it->in_fd);
            close(it->out_fd);
            it = pipe_vector.erase(it);
        } else {
            ++it;
        }
    }
}

void EraseUserPipe(int id) {
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
    if (shm_fifo->fifo[id-1][cur_id-1].is_used) {
        shm_fifo->fifo[id-1][cur_id-1].in_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].out_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].is_used = false;

        unlink(shm_fifo->fifo[id-1][cur_id-1].name);
        memset(&shm_fifo->fifo[id-1][cur_id-1].name, 0, sizeof(shm_fifo->fifo[id-1][cur_id-1].name));
    }
    shmdt(shm_fifo);
}

int ExecCmds() {
    // store arguments
    bool is_first_argv = true;
    bool is_final_argv = false;
    string prog = "";
    vector<string> arguments;
    // pid
    bool is_using_pipe = false;
    bool line_ends = false;
    bool is_in_redirect = false;
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;
    int err_fd = STDERR_FILENO;
    // client status
    int status = 0;
    bool is_in_userpipe = false;
    // client broadcast msg and setup userpipe
    int source_id = -1;
    int target_id = -1;
    // client broadcast msg
    int recv_str_id = -1;
    int send_str_id = -1;

    while (!cmds.empty()) {
        if (!is_in_redirect && !is_in_userpipe) { // init fds
            in_fd = STDIN_FILENO;
            out_fd = STDOUT_FILENO;
            err_fd = STDERR_FILENO;
        }

        if (is_first_argv) {
            prog = cmds.front();
            cmds.pop();
            arguments.clear();
            arguments.push_back(prog);
            
            if (prog == "tell" || prog == "yell") {
                while (!cmds.empty()) {
                    arguments.push_back(cmds.front());
                    cmds.pop();
                }
            }

            is_first_argv = false;
            is_final_argv = cmds.empty();
            is_using_pipe = false;
            line_ends = cmds.empty();
        } else {
            // normal & error pipe
            if (cmds.front().find('|') != string::npos || cmds.front().find('!') != string::npos) {
                int pipe_num;
                
                if (cmds.front().length() == 1) { // simple pipe
                    pipe_num = 1;
                } else { // numbered-pipe
                    string tmp = cmds.front().substr(1);
                    pipe_num = stoi(tmp);
                }

                CreatePipe(pipe_vector, pipe_num, out_fd);

                if (cmds.front().find('!') != string::npos) {
                    err_fd = out_fd;
                }

                cmds.pop();
                is_first_argv = true;
                is_final_argv = true;
                is_using_pipe = true;
                line_ends = cmds.empty();
            // redirection > & <
            } else if (cmds.front() == ">" || cmds.front() == "<") {
                string op = cmds.front();
                cmds.pop();

                int file_fd;
                if (op == ">") {
                    file_fd = open(cmds.front().c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
                    out_fd = file_fd;
                } else {
                    file_fd = open(cmds.front().c_str(), O_RDONLY, 0644);
                    in_fd = file_fd;
                }
                if (file_fd < 0) {
                    cerr << "open file error" << endl;
                }

                cmds.pop();
                is_using_pipe = false;
                is_in_redirect = true;
                is_first_argv = cmds.empty();
                is_final_argv = cmds.empty();
                line_ends = cmds.empty();
            // user pipe (out), ex: >2
            } else if ((cmds.front().find('>') != string::npos) && (cmds.front() != ">")) {
                target_id = stoi(cmds.front().substr(1));
                ClientInfo* shm_cli;

                if (GetClientByID(target_id, shm_cli) != NULL) { // target id exist
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[cur_id-1][target_id-1].in_fd == -1) { // user pipe not exist yet
                        CreateUserPipe(cur_id, target_id, out_fd);

                        is_using_pipe = true;
                        is_in_userpipe = true;
                        cmds.pop();
                        is_first_argv = cmds.empty();
                        is_final_argv = cmds.empty();
                        line_ends = cmds.empty();
                        send_str_id = target_id;
                    } else {
                        fprintf(stderr, "*** Error: the pipe #%d->#%d already exists. ***\n", cur_id, target_id);
                        
                        queue<string> empty;
                        swap(cmds, empty);
                        shmdt(shm_fifo);
                        return 0;
                    }
                    shmdt(shm_fifo);
                } else {
                    cmds.pop();
                    fprintf(stderr, "*** Error: user #%d does not exist yet. ***\n", target_id);
                    target_id = -1;

                    queue<string> empty;
                    swap(cmds, empty);
                    return 0;
                }
            // user pipe (in), ex: <2
            } else if ((cmds.front().find('<') != string::npos) && (cmds.front() != "<")) {
                source_id = stoi(cmds.front().substr(1));
                ClientInfo* shm_cli;

                if (GetClientByID(source_id, shm_cli) != NULL) {
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[source_id-1][cur_id-1].out_fd != -1) { // user pipe exist
                        GetUserPipeFd(source_id, cur_id, in_fd);

                        is_in_userpipe = true;
                        cmds.pop();
                        is_first_argv = cmds.empty();
                        is_final_argv = cmds.empty();
                        line_ends = cmds.empty();
                        recv_str_id = source_id;
                    } else {
                        fprintf(stderr, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", source_id, cur_id);
                        
                        queue<string> empty;
                        swap(cmds, empty);
                        shmdt(shm_fifo);
                        return 0;
                    }
                    shmdt(shm_fifo);
                } else {
                    cmds.pop();
                    fprintf(stderr, "*** Error: user #%d does not exist yet. ***\n", target_id);
                    source_id = -1;
                    
                    queue<string> empty;
                    swap(cmds, empty);
                    return 0;
                }
            } else { // other cmds
                arguments.push_back(cmds.front());
                cmds.pop();
                is_using_pipe = false;
                is_final_argv = cmds.empty();
                line_ends = cmds.empty();
            }
        }

        if (is_final_argv) { // execute
            // broadcast send and recv
            if (recv_str_id != -1) {
                if (!terminal_input.empty() && terminal_input.back() == '\r') terminal_input.pop_back();
                recv_str_id = -1;
                Broadcast("recv", terminal_input, cur_id, source_id);
                usleep(500);
            }
            if (send_str_id != -1) {
                if (!terminal_input.empty() && terminal_input.back() == '\r') terminal_input.pop_back();
                send_str_id = -1;
                Broadcast("send", terminal_input, cur_id, target_id);
                usleep(500);
            }
            
            // pipe: get in pipe (count == 0)
            bool need_close_pipe = GetPipeFd(in_fd);

            status = ExecCmd(arguments, in_fd, out_fd, err_fd, line_ends, is_using_pipe, target_id);

            // pipe
            ErasePipefd();
            CountdownPipefd();
            if (need_close_pipe) close(in_fd);
            if (target_id > 0) target_id = -1;
            if (source_id > 0) { // erase the user pipe read from
                EraseUserPipe(source_id);
                source_id = -1;
            }
            is_final_argv = false;
            is_in_redirect = false;
            is_in_userpipe = false;
        }
    }
    return status;
}

void PrintWelcome() {
    string msg = "";
    msg += "****************************************\n";
    msg += "** Welcome to the information server. **\n";
    msg += "****************************************\n";
    cout << msg;
}

int ClientExec(int id) {
    cur_id = id;  // current id
    clearenv();
    SetEnv("PATH", "bin:.");
    signal(SIGUSR1, SigHandler); // receive messages from others
	signal(SIGUSR2, SigHandler); // open user pipe to read
	signal(SIGINT, SigHandler);
	signal(SIGQUIT, SigHandler);
	signal(SIGTERM, SigHandler);

    PrintWelcome();
    Broadcast("login", "", cur_id, -1);
    pid_vector.clear();

    while (true) {
        cout << "% ";

        getline(cin, terminal_input);
        if(cin.eof()) {
            cout << endl;
            return 1;
        }
        if(terminal_input.empty()) continue;

        stringstream ss(terminal_input);
        string tmp = "";
        while (ss >> tmp) {
            cmds.push(tmp);
        }

        if (ExecCmds() == -1) return -1;
    }
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

void InitSHM() {
    g_shmid_cli = shmget(SHM_KEY, sizeof(ClientInfo) * MAX_USER, 0666 | IPC_CREAT);
	if (g_shmid_cli < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(EXIT_FAILURE);
	}
    g_shmid_msg = shmget(SHM_MSG_KEY, sizeof(char) * MAX_MSG_LEN, 0666 | IPC_CREAT);
	if (g_shmid_msg < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(EXIT_FAILURE);
	}
    g_shmid_fifo = shmget(SHM_FIFO_KEY, sizeof(FIFOInfo), 0666 | IPC_CREAT);
	if (g_shmid_fifo < 0) {
		cerr << "Error: init_shm() failed" <<endl;
		exit(EXIT_FAILURE);
	}

	ClientInfo *shm = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		shm[i].valid = 0;
	}
	shmdt(shm);

    FIFOInfo *shm_fifo = GetFIFOSHM(g_shmid_fifo);
	for (int i = 0; i < MAX_USER; ++i) {
        for (int j = 0; j < MAX_USER; ++j) {
            shm_fifo->fifo[i][j].in_fd = -1;
            shm_fifo->fifo[i][j].out_fd = -1;
            shm_fifo->fifo[i][j].is_used = false;
            memset(&shm_fifo->fifo[i][j].name, 0, sizeof(shm_fifo->fifo[i][j].name));
        }
	}
	shmdt(shm_fifo);
}

int GetIDFromSHM() {
	ClientInfo *shm = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		if (!shm[i].valid) {
            shm[i].valid = 1;
			shmdt(shm);
			return i + 1;
		}
	}
	shmdt(shm);
    return -1;
}

void AddClient(int id, int sockfd, sockaddr_in address) {
    int shm_idx = id - 1;
    ClientInfo* shm = GetCliSHM(g_shmid_cli);
	shm[shm_idx].valid = 1;
    shm[shm_idx].id = id;
	shm[shm_idx].pid = getpid();
	shm[shm_idx].port = ntohs(address.sin_port);
    strncpy(shm[shm_idx].user_ip, inet_ntoa(address.sin_addr), INET_ADDRSTRLEN);
	strcpy(shm[shm_idx].user_name, "(no name)");
	shmdt(shm);
}

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_multi_proc [port]" << endl;
        exit(EXIT_FAILURE);
    }

    int server_sockfd = TCPconnect(atoi(argv[1]));

    signal(SIGCHLD, ServerSigHandler);
	signal(SIGINT, ServerSigHandler);
	signal(SIGQUIT, ServerSigHandler);
	signal(SIGTERM, ServerSigHandler);

    InitSHM();

    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sockfd = accept(server_sockfd, (struct sockaddr *) &client_addr, &addr_len);
        if (client_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }

        int childpid = fork();
        while (childpid < 0) {
            childpid = fork();
        }
        if (childpid == 0) {
            dup2(client_sockfd, STDIN_FILENO);
            dup2(client_sockfd, STDOUT_FILENO);
            dup2(client_sockfd, STDERR_FILENO);
            close(client_sockfd);
            close(server_sockfd);

            int client_id = GetIDFromSHM();
            if (client_id < 0) {
                cerr << "Error: get client id failed" << endl;
                exit(EXIT_FAILURE);
            }
            AddClient(client_id, client_sockfd, client_addr);
            if (ClientExec(client_id) == -1) { // -1 represent exit
                Broadcast("logout", "", cur_id, -1);

                // clean client info
                ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
                shm_cli[cur_id-1].valid = 0;
                shmdt(shm_cli);

                // clean FIFO info
                ResetCliFIFOSHM();

                close(STDIN_FILENO);
                close(STDOUT_FILENO);
                close(STDERR_FILENO);
                exit(EXIT_SUCCESS);
            }
        } else {
            close(client_sockfd);
        }
    }
    close(server_sockfd);
}