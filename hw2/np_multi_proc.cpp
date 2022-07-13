#include <sstream>
#include <stdio.h>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <queue>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
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

int TCPconnect(uint16_t port);
ClientInfo* GetCliSHM(int g_shmid_cli);
ClientInfo* GetClientByID(int id, ClientInfo*);
char* GetMsgSHM(int g_shmid_msg);
FIFOInfo* GetFIFOSHM(int g_shmid_fifo);
int GetIDFromSHM();
void Broadcast(string action, string msg, int cur_id, int target_id);
void BroadcastOne(string action, string msg, int cur_id, int target_id);
void PrintWelcome();
void SigHandler(int sig);
void AddClient(int id, int sockfd, sockaddr_in address);
void ServerSigHandler (int sig);
void ResetCliSHM(int g_shmid_cli);
void ResetFIFOSHM(int g_shmid_fifo);
void InitSHM();
void DelSHM();

class Shell {
    private:
        // path
        vector<string> path_vector_;
        // commands
        string terminal_input_;
        queue<string> cmds_;
        // pid
        vector<pid_t> pid_vector_;
        // numbered pipe
        vector<PipeFd> pipe_vector_;
    public:
        void Exec(int);
        // built-in ops
        vector<string> Split(string, string);
        void SetEnv(string, string);
        void PrintEnv(string);
        // command ops
        void ParseArgs();
        int ExecCmds();
        bool IsQueueEmpty(queue<string>);
        int ExecCmd(vector<string> &, bool, int, int, int, bool, bool, int);
        bool IsExecutable(string, vector<string>&);
        // pipe ops
        void CreatePipe(vector<PipeFd>&, int, int&);
        void CountdownPipefd(vector<PipeFd>&);
        bool GetPipeFd(vector<PipeFd>&, int&);
        void BindPipeFd(int, int&);
        void ConnectPipeFd(int, int, int);
        void ErasePipefd(vector<PipeFd>&);
        // pid ops
        static void ChildHandler(int);
        // client ops
        int ClientExec(int);
        void Who(void);
        void Yell(string);
        void Tell(int, string);
        void Name(string);
        void SetAllEnv(void);
        void EraseEnv(string);
        // client user pipe
        void CreateUserPipe(int, int, int&);
        void GetUserPipeFd(int, int, int&);
        void SetUserPipeOut(int send_id, int& out_fd);
        void EraseUserPipe(int);
};

int Shell::ClientExec(int id) {
    cur_id = id;  // current id
    clearenv();
    SetEnv("PATH", "bin:.");
    // signal handler
    signal(SIGUSR1, SigHandler);// receive messages from others
	signal(SIGUSR2, SigHandler);// open fifos to read
	signal(SIGINT, SigHandler);
	signal(SIGQUIT, SigHandler);
	signal(SIGTERM, SigHandler);

    PrintWelcome();
    Broadcast("login", "", cur_id, -1);
    pid_vector_.clear();

    while (true) {
        cout << "% ";

        getline(cin, terminal_input_);
        if(cin.eof()) {
            cout << endl;
            return 1;
        }
        if(terminal_input_.empty()) {
            continue;
        }

        ParseArgs();

        if (ExecCmds() == -1) {
            return -1;
        }
    }
}
void Shell::SetEnv(string var, string val) {
    setenv(var.c_str(), val.c_str(), 1);
    if (var == "PATH") {
        path_vector_.clear();
        vector<string> res = Split(val, ":");
        for(vector<string>::iterator it = res.begin(); it != res.end(); ++it) {
            path_vector_.push_back(*it);
        }
    }
}

vector<string> Shell::Split(string s, string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    string token;
    vector<string> res;
    while ((pos_end = s.find(delimiter, pos_start)) != string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }
    res.push_back(s.substr (pos_start));
    return res;
}
void Shell::PrintEnv(string var) {
    char* val = getenv(var.c_str());
    if (val != NULL) {
        cout << val << endl;
    }
}
void Shell::Who(void) {
    int temp_id = 0;
    ClientInfo* shm_cli;
    ClientInfo* cur_cli = GetClientByID(cur_id, shm_cli);

    cout << "<ID>\t" << "<nickname>\t" << "<IP:port>\t"<<"<indicate me>"<< endl;

    for (size_t id = 0; id < MAX_USER; ++id) {
        if (GetClientByID(id + 1, shm_cli) != NULL) {
            temp_id = id + 1;
            ClientInfo* temp = GetClientByID(temp_id, shm_cli);
            cout << temp->id << "\t" << temp->user_name << "\t" << temp->user_ip << ":" << temp->port;

            if (temp_id == cur_cli->id) {
                cout << "\t" << "<-me" << endl;
            } else {
                cout << "\t" << endl;
            }
        }
    }
    shmdt(shm_cli);
}
void Shell::Yell(string msg) {
    Broadcast("yell", msg, cur_id, -1);
}
void Shell::Tell(int target_id, string msg) {
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    if(shm_cli[target_id-1].valid != 0) {
        BroadcastOne("tell", msg, cur_id, target_id);
    } else {
        cerr << "*** Error: user #" << to_string(target_id) << " does not exist yet. ***" << endl;
    }
    shmdt(shm_cli);
}
void Shell::Name(string name) {
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    for (size_t i = 0; i < MAX_USER; ++i) {
        if (shm_cli[i].user_name == name) {
            cout << "*** User '" + name + "' already exists. ***" << endl;
            return;
        }
    }
    strcpy(shm_cli[cur_id-1].user_name, name.c_str());
    shmdt(shm_cli);
    Broadcast("name", "", cur_id, -1);
}

void Shell::ParseArgs() {
    istringstream in(terminal_input_);
    string t;
    while (in >> t) {
        cmds_.push(t);
    }
}
int Shell::ExecCmds() {
    // store arguments
    bool is_first_argv = true;
    bool is_final_argv = false;
    string prog;
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

    while (!cmds_.empty()) {
        // init fd
        if (!is_in_redirect && !is_in_userpipe) {
            in_fd = STDIN_FILENO;
            out_fd = STDOUT_FILENO;
            err_fd = STDERR_FILENO;
        }

        if (is_first_argv) {
            prog = cmds_.front();
            cmds_.pop();
            arguments.clear();
            arguments.push_back(prog);
            
            if (prog == "tell" || prog == "yell") {
                while (!cmds_.empty()) {
                    arguments.push_back(cmds_.front());
                    cmds_.pop();
                }
            }

            is_first_argv = false;
            is_final_argv = IsQueueEmpty(cmds_);
            is_using_pipe = false;
            if (cmds_.empty()) {
                line_ends = true;
            }
        } else {
            // normal & error pipe
            if (cmds_.front().find('|') != string::npos || cmds_.front().find('!') != string::npos) {
                int pipe_num;
                char pipe_char[5];
                
                // simple pipe
                if (cmds_.front().length() == 1) {
                    pipe_num = 1;
                // numbered-pipe
                } else {
                    for (int i = 1; i < (int)cmds_.front().length(); ++i) {
                        pipe_char[i-1] = cmds_.front()[i];
                        pipe_char[i] = '\0';
                    }
                    pipe_num = atoi(pipe_char);
                }

                CreatePipe(pipe_vector_, pipe_num, out_fd);

                if (cmds_.front().find('!') != string::npos) {
                    err_fd = out_fd;
                }

                is_first_argv = true;
                is_final_argv = true;
                is_using_pipe = true;
                if (cmds_.empty()) {
                    line_ends = true;
                }
                cmds_.pop();
            // redirection > & <
            } else if (cmds_.front() == ">" || cmds_.front() == "<") {
                string op = cmds_.front();
                cmds_.pop();

                int file_fd;
                if (op == ">") {
                    file_fd = open(cmds_.front().c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
                } else {
                    file_fd = open(cmds_.front().c_str(), O_RDONLY, 0644);
                }
                if (file_fd < 0) {
                    cerr << "open file error" << endl;
                }
                cmds_.pop();

                if (op == ">") {
                    out_fd = file_fd;
                } else {
                    in_fd = file_fd;
                }
                
                is_using_pipe = false;
                is_in_redirect = true;
                if (cmds_.empty()) {
                    line_ends = true;
                    is_first_argv = true;
                    is_final_argv = true;
                }
            // named pipe (out), ex: >2
            } else if ((cmds_.front().find('>') != string::npos) && (cmds_.front() != ">")) {
                char user_char[3];

                for (int i = 1; i < (int)cmds_.front().length(); ++i) {
                    user_char[i-1] = cmds_.front()[i];
                    user_char[i] = '\0';
                }
                
                target_id = atoi(user_char);
                ClientInfo* shm_cli;

                // target id not exist
                if (GetClientByID(target_id, shm_cli) == NULL) {
                    cmds_.pop();
                    
                    cout << "*** Error: user #" << target_id << " does not exist yet. ***" << endl;
                    target_id = -1;
                    
                    queue<string> empty;
                    swap(cmds_, empty);
                    return 0;
                // target id exists
                } else {
                    // check if user pipe already exists
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[cur_id-1][target_id-1].in_fd != -1) {
                        cout << "*** Error: the pipe #" << cur_id << "->#" << target_id;
                        cout << " already exists. ***" << endl;
                        
                        queue<string> empty;
                        swap(cmds_, empty);
                        return 0;
                    }
                    shmdt(shm_fifo);

                    // mkFIFO and record FIFOname
                    CreateUserPipe(cur_id, target_id, out_fd);
                    is_using_pipe = true;
                    is_in_userpipe = true;
                    cmds_.pop();
                    if (cmds_.empty()) {
                        line_ends = true;
                        is_first_argv = true;
                        is_final_argv = true;
                    }
                    send_str_id = target_id;
                }
            // named pipe (in), ex: <2
            } else if ((cmds_.front().find('<') != string::npos) && (cmds_.front() != "<")) {
                char user_char[3];

                for (int i = 1; i < (int)cmds_.front().length(); ++i) {
                    user_char[i-1] = cmds_.front()[i];
                    user_char[i] = '\0';
                }

                source_id = atoi(user_char);
                ClientInfo* shm_cli;
                // target id does not exist
                if (GetClientByID(source_id, shm_cli) == NULL) {
                    cmds_.pop();

                    cout << "*** Error: user #" << source_id << " does not exist yet. ***" << endl;
                    source_id = -1;
                    
                    queue<string> empty;
                    swap(cmds_, empty);
                    return 0;
                // target id exists
                } else {
                    // check if user pipe already exists
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[source_id-1][cur_id-1].out_fd == -1) {
                        // cannot find any userpipe's target id is current client id
                        cout << "*** Error: the pipe #" << source_id << "->#" << cur_id;
                        cout << " does not exist yet. ***" << endl;
                        
                        queue<string> empty;
                        swap(cmds_, empty);
                        return 0;
                    }
                    shmdt(shm_fifo);

                    GetUserPipeFd(source_id, cur_id, in_fd);
                    is_in_userpipe = true;
                    cmds_.pop();
                    if (cmds_.empty()) {
                        line_ends = true;
                        is_first_argv = true;
                        is_final_argv = true;
                    }
                    recv_str_id = source_id;
                }
            } else {
                arguments.push_back(cmds_.front());
                cmds_.pop();
                is_final_argv = IsQueueEmpty(cmds_);
                is_using_pipe = false;
                if (cmds_.empty()) {
                    line_ends = true;
                }
            }

        // execute
        } if (is_final_argv) {
            // broadcast send and recv
            if (recv_str_id != -1) {
                string line_input = terminal_input_.substr(0, terminal_input_.length());
                if (line_input.back()=='\r') {
                    line_input.pop_back();
                }
                recv_str_id = -1;
                Broadcast("recv", line_input, cur_id, source_id);
                usleep(500);
            }
            if (send_str_id != -1) {
                string line_input = terminal_input_.substr(0, terminal_input_.length());
                if (line_input.back()=='\r') {
                    line_input.pop_back();
                }
                send_str_id = -1;
                Broadcast("send", line_input, cur_id, target_id);
                usleep(500);
            }
            
            // pipe: get pipe (count == 0)
            bool need_close_pipe = GetPipeFd(pipe_vector_, in_fd);

            // execute
            status = ExecCmd(arguments, IsExecutable(prog, path_vector_), in_fd, out_fd, err_fd, line_ends, is_using_pipe, target_id);

            // pipe
            ErasePipefd(pipe_vector_);
            CountdownPipefd(pipe_vector_);
            if (need_close_pipe) {
                close(in_fd);
            }
            if (target_id > 0) {
                target_id = -1;
            }
            // if userpipe in , then erase
            if (source_id > 0) {
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
bool Shell::IsQueueEmpty(queue<string> cmds) {
    if (cmds.empty()) return true;
    return false;
}
int Shell::ExecCmd(vector<string> &arguments, bool is_executable, int in_fd, int out_fd, int err_fd, bool line_ends, bool is_using_pipe, int target_id) {
    char *args[arguments.size() + 1];
    for (size_t i = 0; i < arguments.size(); ++i) {
        args[i] = new char[arguments[i].size() + 1];
        strcpy(args[i], arguments[i].c_str());
    }

    // built-in
    string prog(args[0]);
    if (prog == "printenv") {
        PrintEnv(args[1]);
        return 0;
    } else if (prog == "setenv") {
        SetEnv(args[1], args[2]);
        return 0;
    } else if (prog == "who") {
        Who();
        return 0;
    } else if (prog == "yell") {
        string msg = "";

        for (size_t i = 1; i < arguments.size(); ++i) {
            msg += string(args[i]) + " ";
        }

        msg.pop_back();
        Yell(msg);
        return 0;
    } else if (prog == "tell") {
        string msg = "";

        for (size_t i = 2; i < arguments.size(); ++i) {
            msg += string(args[i]) + " ";
        }

        msg.pop_back();
        Tell(stoi(args[1]), msg);
        return 0;
    } else if (prog == "name") {
        Name(string(args[1]));
        return 0;
    } else if (prog == "exit") {
        return -1;
    }

    // not built-in
    signal(SIGCHLD, ChildHandler);
    pid_t child_pid;
    child_pid = fork();
    while (child_pid < 0) {
        usleep(1000);
        child_pid = fork();
    }

    // child process
    if (child_pid == 0) {
        // current client open FIFO and record write fd
        if (target_id > 0) {
            SetUserPipeOut(target_id, out_fd);
        }

        // pipe ops
        ConnectPipeFd(in_fd, out_fd, err_fd);
        if (!is_executable) {
            cerr << "Unknown command: [" << args[0] << "]." << endl;
            exit(0);
        } else {
            args[arguments.size()] = NULL;
            if(execvp(args[0], args) < 0) {
                cerr << "execl error" << endl;
                exit(0);
            }
            cerr << "execvp" << endl;
            exit(0);
        }
    // parent process
    } else {
        if (line_ends) {
            if (!is_using_pipe) {
                int status;
                waitpid(child_pid, &status, 0);
            }
        }
    }
    return 0;
}
bool Shell::IsExecutable(string prog, vector<string> &path_vector_) {
    if (prog == "printenv" || prog == "setenv" || prog == "exit" ||
        prog == "who" || prog == "tell" || prog == "yell" || prog == "name") {
        return true;
    }

    bool is_executable;
    string path;
    vector<string>::iterator iter = path_vector_.begin();
    while (iter != path_vector_.end()) {
        path = *iter;
        path = path + "/" + prog;
        is_executable = (access(path.c_str(), 0) == 0);
        if (is_executable) {
            return true;
        }
        ++iter;
    }
    return false;
}

void Shell::CreatePipe(vector<PipeFd> &pipe_vector_, int pipe_num, int &in_fd) {
    // check if pipe to same pipe
    // has same pipe => reuse old pipe (multiple write, one read)
    // no same pipe => create new pipe (one write, one read)
    bool has_same_pipe = false;

    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while(iter != pipe_vector_.end()) {
        if ((*iter).count == pipe_num) {
            has_same_pipe = true;
            in_fd = (*iter).in_fd;
        }
        ++iter;
    }

    if (has_same_pipe) {
        return;
    }

    // create pipe
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        cerr << "pipe error" << endl;
        exit(0);
    }
    PipeFd new_pipefd;
    new_pipefd.in_fd = pipe_fd[1];  // write fd
    new_pipefd.out_fd = pipe_fd[0];  // read fd
    new_pipefd.count = pipe_num;
    pipe_vector_.push_back(new_pipefd);
    in_fd = pipe_fd[1];
}
void Shell::CountdownPipefd(vector<PipeFd> &pipe_vector_) {
    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) {
        --(*iter).count;
        ++iter;
    }
}
void Shell::ErasePipefd(vector<PipeFd> &pipe_vector_) {
    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) {
        if ((*iter).count == 0) {
            close((*iter).in_fd);
            close((*iter).out_fd);
            pipe_vector_.erase(iter);
        } else {
            ++iter;
        }
    }
}
bool Shell::GetPipeFd(vector<PipeFd> &pipe_vector_, int& in_fd) {
    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) {
        if ((*iter).count == 0) {
            close((*iter).in_fd);
            in_fd = (*iter).out_fd;
            return true;
        }
        ++iter;
    }
    return false;
}
void Shell::ConnectPipeFd(int in_fd, int out_fd, int err_fd) {
    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
    }
    if (err_fd != STDERR_FILENO) {
        dup2(err_fd, STDERR_FILENO);
    }
    if (in_fd != STDIN_FILENO) {
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        close(out_fd);
    }
    if (err_fd != STDERR_FILENO) {
        close(err_fd);
    }
};

void Shell::ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

void Shell::CreateUserPipe(int cur_id, int target_id, int &out_fd) {
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);
    
    if (mkfifo(fifopath, 0666 | S_IFIFO) < 0) {
        cerr << "mkfifo error" << endl;
        exit(0);
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
void Shell::SetUserPipeOut(int target_id, int& out_fd) {
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);
    ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    out_fd = open(fifopath, O_WRONLY);
    shm_fifo->fifo[cur_id-1][target_id-1].in_fd = out_fd;

    shmdt(shm_cli);
    shmdt(shm_fifo);
}
void Shell::GetUserPipeFd(int source_id, int cur_id, int& in_fd) {
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
void Shell::EraseUserPipe(int id) {
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    if (shm_fifo->fifo[id-1][cur_id-1].is_used) {
        shm_fifo->fifo[id-1][cur_id-1].in_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].out_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].is_used = false;

        unlink(shm_fifo->fifo[id-1][cur_id-1].name);
        memset(&shm_fifo->fifo[id-1][cur_id-1].name, 0, sizeof(shm_fifo->fifo[id-1][cur_id-1].name));
    }
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
void PrintWelcome() {
    string msg = "";
    msg += "****************************************\n";
    msg += "** Welcome to the information server. **\n";
    msg += "****************************************\n";
    cout << msg;
}
void Broadcast(string action, string msg, int cur_id, int target_id) {
    string send_msg = "";
    ClientInfo* shm_cli;
    ClientInfo* cur_cli = GetClientByID(cur_id, shm_cli);
    ClientInfo* target_cli;
    if (target_id != -1) {
        target_cli = GetClientByID(target_id, shm_cli);
    } else {
        target_cli = NULL;
    }
    if (cur_cli == NULL) {
        cout << "cannot find client id: " << cur_id << endl;
        return;
    }

    // broadcast to all
    if (action == "login") {
        send_msg = "*** User '(no name)' entered from " + string(cur_cli->user_ip) + ":" + to_string(cur_cli->port) + ". ***\n";
    } else if (action == "logout") {
        send_msg = "*** User '" + string(cur_cli->user_name) + "' left. ***\n";
    } else if (action == "name") {
        send_msg = "*** User from " + string(cur_cli->user_ip) + ":" + to_string(cur_cli->port);
        send_msg += " is named '" + string(cur_cli->user_name) + "'. ***\n";
    } else if (action == "yell") {
        send_msg = "*** " + string(cur_cli->user_name) + " yelled ***: " + msg + "\n";   
    } else if (action == "send") {
        send_msg = "*** " + string(cur_cli->user_name) + " (#" + to_string(cur_cli->id) + ") just piped '";
        send_msg += msg + "' to " + string(target_cli->user_name) + " (#" + to_string(target_cli->id) + ") ***\n";
    } else if (action == "recv") {
        send_msg = "*** " + string(cur_cli->user_name) + " (#" + to_string(cur_cli->id);
        send_msg += ") just received from " + string(target_cli->user_name) + " (#";
        send_msg += to_string(target_cli->id) + ") by '" + msg + "' ***\n";
    }

    char* shm_msg = GetMsgSHM(g_shmid_msg);
    sprintf(shm_msg, "%s", send_msg.c_str());
    shmdt(shm_msg);

    usleep(500);

    // shot down process
    shm_cli = GetCliSHM(g_shmid_cli);
    for (int i = 0; i < MAX_USER; ++i) {
		if (shm_cli[i].valid == 1) {
            kill(shm_cli[i].pid, SIGUSR1);
        }
	}
	shmdt(shm_cli);
    shmdt(shm_msg);
}

void BroadcastOne(string action, string msg, int cur_id, int target_id) {
    string send_msg = "";
    ClientInfo* shm = GetCliSHM(g_shmid_cli);
    ClientInfo* cur_cli = GetClientByID(cur_id, shm);

    // broadcast to one
    if (action == "tell") {
        send_msg = "*** " + string(cur_cli->user_name) + " told you ***: " + msg + "\n";
    }

    char* shm_msg = GetMsgSHM(g_shmid_msg);
    sprintf(shm_msg, "%s", send_msg.c_str());
    shmdt(shm_msg);
    usleep(500);

    shm = GetCliSHM(g_shmid_cli);
    kill(shm[target_id-1].pid, SIGUSR1);
    shmdt(shm);
}

void ResetCliSHM(int g_shmid_cli) {
	ClientInfo *shm = (ClientInfo*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (ClientInfo*)0) {
		cerr << "Error: shmat() failed" << endl;
		exit(1);
	}
	for (int i = 0; i < MAX_USER; ++i) {
		shm[i].valid = 0;
	}
	shmdt(shm);
}
void ResetFIFOSHM(int g_shmid_fifo) {
	FIFOInfo *shm_fifo = GetFIFOSHM(g_shmid_fifo);
	for (int i = 0; i < MAX_USER; ++i) {
        for (int j = 0; j < MAX_USER; ++j) {
            shm_fifo->fifo[i][j].in_fd = -1;
            shm_fifo->fifo[i][j].out_fd = -1;
            shm_fifo->fifo[i][j].is_used = 0;
            char name[MAX_PATH_SIZE];
            memset(&shm_fifo->fifo[i][j].name, 0, sizeof(name));
        }
	}
	shmdt(shm_fifo);
}
void InitSHM() {
	int shm_size = sizeof(ClientInfo) * MAX_USER;
	int msg_size = sizeof(char) * MAX_MSG_LEN;
    int fifo_size = sizeof(FIFOInfo);

    int shmid_cli = shmget(SHM_KEY, shm_size, 0666 | IPC_CREAT);
	if (shmid_cli < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(1);
	}
    int shmid_msg = shmget(SHM_MSG_KEY, msg_size, 0666 | IPC_CREAT);
	if (shmid_msg < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(1);
	}
    int shmid_fifo = shmget(SHM_FIFO_KEY, fifo_size, 0666 | IPC_CREAT);
	if (shmid_fifo < 0) {
		cerr << "Error: init_shm() failed" <<endl;
		exit(1);
	}

	ResetCliSHM(shmid_cli);
    ResetFIFOSHM(shmid_fifo);

	// update global var
	g_shmid_cli = shmid_cli;
	g_shmid_msg = shmid_msg;
    g_shmid_fifo = shmid_fifo;
}
ClientInfo* GetCliSHM(int g_shmid_cli) {
    ClientInfo* shm = (ClientInfo*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (ClientInfo*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}
char* GetMsgSHM(int g_shmid_msg) {
    char* shm = (char*)shmat(g_shmid_msg, NULL, 0);
	if (shm < (char*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}
FIFOInfo* GetFIFOSHM(int g_shmid_fifo) {
    FIFOInfo* shm = (FIFOInfo*)shmat(g_shmid_fifo, NULL, 0);
	if (shm < (FIFOInfo*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}
// get available ID
int GetIDFromSHM() {
	ClientInfo *shm = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		if (!shm[i].valid) {
            shm[i].valid = 1;
			shmdt(shm);
			return (i+1);
		}
	}
	shmdt(shm);
    return -1;
}
ClientInfo* GetClientByID(int id, ClientInfo* shm_cli) {
    shm_cli = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		if ((shm_cli[i].id == id) && (shm_cli[i].valid == 1)) {
            ClientInfo* res = &shm_cli[i];
			return res;
		}
	}
    return NULL;
}
void SigHandler(int sig) {
    // receive messages from others
	if (sig == SIGUSR1) {
		char* msg = (char*)shmat(g_shmid_msg, NULL, 0);
        if (msg < (char*)0) {
            cerr << "Error: shmat() failed" << endl;
            exit(1);
        }
        if (write(STDOUT_FILENO, msg, strlen(msg)) < 0) {
            cerr << "Error: broadcast_catch() failed" << endl;
        }
        shmdt(msg);
    // open FIFOs to read
	} else if (sig == SIGUSR2) {
        FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
		int	i;
		for (i = 0; i < MAX_USER; ++i) {
			if (shm_fifo->fifo[i][cur_id-1].out_fd == -1 && shm_fifo->fifo[i][cur_id-1].name[0] != 0) {
                shm_fifo->fifo[i][cur_id-1].out_fd = open(shm_fifo->fifo[i][cur_id-1].name, O_RDONLY);
            }
		}
        shmdt(shm_fifo);
    // clean client
	} else if (sig == SIGINT || sig == SIGQUIT || sig == SIGTERM) {
        Broadcast("logout", "", cur_id, -1);
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
	}
	signal(sig, SigHandler);
}

void AddClient(int id, int sockfd, sockaddr_in address) {
	ClientInfo* shm;
    int shm_idx = id - 1;
	if (id < 0) {
		cerr << "Error: get_new_id() failed" << endl;
        exit(1);
	}
    shm = (ClientInfo*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (ClientInfo*)0) {
		cerr << "Error: init_new_client() failed" << endl;
        exit(1);
	}
	shm[shm_idx].valid = 1;
    shm[shm_idx].id = id;
	shm[shm_idx].pid = getpid();
	shm[shm_idx].port = ntohs(address.sin_port);
    strncpy(shm[shm_idx].user_ip, inet_ntoa(address.sin_addr), INET_ADDRSTRLEN);
	strcpy(shm[shm_idx].user_name, "(no name)");

	shmdt(shm);
}
void DelSHM() {
	// delete all
	shmctl(g_shmid_cli, IPC_RMID, NULL);
	shmctl(g_shmid_msg, IPC_RMID, NULL);
    shmctl(g_shmid_fifo, IPC_RMID, NULL);
}
void ServerSigHandler(int sig) {
	if (sig == SIGCHLD) {
		while (waitpid (-1, NULL, WNOHANG) > 0);
	} else if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM) {
		DelSHM();
		exit (0);
	}
	signal (sig, ServerSigHandler);
}

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_multi_proc [port]" << endl;
        exit(1);
    }

    int server_sockfd, client_sockfd, childpid;
    socklen_t addr_len;
    struct sockaddr_in cli_addr;

    int port = atoi(argv[1]);
    server_sockfd = TCPconnect(port);
    cout << "Server sockfd: " << server_sockfd << " port: " << port << endl;

    signal (SIGCHLD, ServerSigHandler);
	signal (SIGINT, ServerSigHandler);
	signal (SIGQUIT, ServerSigHandler);
	signal (SIGTERM, ServerSigHandler);

    InitSHM();

    while (true) {
        addr_len = sizeof(cli_addr);
        client_sockfd = accept(server_sockfd, (struct sockaddr *) &cli_addr, &addr_len);
        if (client_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }
        cout << "client sockfd: " << client_sockfd << endl;

        childpid = fork();
        while (childpid < 0) {
            childpid = fork();
        }
        if (childpid == 0) {
            dup2(client_sockfd, STDIN_FILENO);
            dup2(client_sockfd, STDERR_FILENO);
            dup2(client_sockfd, STDOUT_FILENO);
            close(client_sockfd);
            close(server_sockfd);

            int client_id = GetIDFromSHM();
            AddClient(client_id, client_sockfd, cli_addr);
            Shell shell;
            if (shell.ClientExec(client_id) == -1) { // -1 represent exec
                Broadcast("logout", "", cur_id, -1);

                // clean client info
                ClientInfo* shm_cli = GetCliSHM(g_shmid_cli);
                shm_cli[cur_id-1].valid = 0;
                shmdt(shm_cli);

                // clean FIFO info
                FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                for (size_t i = 0; i < MAX_USER; i++) {
                    if (shm_fifo->fifo[i][cur_id-1].out_fd != -1) {
                        // read out message in the unused fifo
                        char buf[1024];
                        while (read(shm_fifo->fifo[i][cur_id-1].out_fd, &buf, sizeof(buf)) > 0){}
                        shm_fifo->fifo[i][cur_id-1].out_fd = -1;
                        shm_fifo->fifo[i][cur_id-1].in_fd = -1;
                        shm_fifo->fifo[i][cur_id-1].is_used = false;
                        unlink(shm_fifo->fifo[i][cur_id-1].name);
                        memset(shm_fifo->fifo[i][cur_id-1].name, 0, sizeof(shm_fifo->fifo[cur_id-1][i].name));
                    }
                }
                shmdt(shm_fifo);

                close(STDIN_FILENO);
                close(STDOUT_FILENO);
                close(STDERR_FILENO);
                exit(0);
            }
        } else {
            close(client_sockfd);
        }
    }
    close(server_sockfd);
}