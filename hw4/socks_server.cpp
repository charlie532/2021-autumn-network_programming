#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <boost/asio.hpp>
#include <sys/wait.h>

using namespace std;
using boost::asio::ip::tcp;

boost::asio::io_service io_service;

class session: public std::enable_shared_from_this<session> {
public:
    session(tcp::socket socket): in_socket_(std::move(socket)), out_socket_(io_service) {
        memset(in_data_, '\0', max_length);
        memset(out_data_, '\0', max_length);
        memset(request_data_, '\0', max_length);
    }

    void start() {
        do_read_socks4_request();
    }

private:
    bool firewall() {
        string temp, head, op, permit_ip, request_ip_token[4];
        stringstream ss;
        ifstream conf("./socks.conf");

        if (conf.is_open() == false) {
            return false;
        }
        ss << request_endpoint_.address().to_string();
        int i = 0;
        while (getline(ss, temp, '.')) {
            request_ip_token[i] = temp;
            ++i;
        }

        while (conf >> head >> op >> permit_ip) {
            bool permit = true;

            if ((op == "c" && request_data_[1] != 1) || (op == "b" && request_data_[1] != 2)) {
                continue;
            } else {
                ss.clear();
                ss << permit_ip;
                int i = 0;
                while(getline(ss, temp, '.')) {
                    if (temp == "*") {
                        continue;
                    } else if (request_ip_token[i] != temp) {
                        permit = false;
                    }
                    ++i;
                }
            }
            
            if (permit) {
                conf.close();
                return true;
            }
        }
        conf.close();
        return false;
    }

    void do_connect() {
		out_socket_.connect(request_endpoint_);
    }

    void do_bind() {
        auto self(shared_from_this());
        tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), 0));
        do_write_socks4_reply(90, acceptor.local_endpoint());
        acceptor.accept(out_socket_);
        do_write_socks4_reply(90, acceptor.local_endpoint());
    }

    void do_resolve() {
        string port, ip;
        port = to_string(ntohs(*((uint16_t*)&request_data_[2])));
        if (request_data_[4] == 0 && request_data_[5] == 0 && request_data_[6] == 0) {
            ip = string(&request_data_[9]);
        } else {
            ip = to_string((uint8_t)request_data_[4]) + '.' + to_string((uint8_t)request_data_[5]) + '.' + to_string((uint8_t)request_data_[6]) + '.' + to_string((uint8_t)request_data_[7]);
        }

        tcp::resolver resolver(io_service);
        tcp::resolver::query query(ip, port);
        tcp::resolver::iterator it = resolver.resolve(query);
        this->request_endpoint_ = it->endpoint();
    }

    void do_close() {
        in_socket_.close();
        out_socket_.close();
    }

    void output_request_result(const char reply[]) {
        cout << "<S_IP>: " << in_socket_.remote_endpoint().address().to_string() << endl;
        cout << "<S_PORT>: " <<  to_string(in_socket_.remote_endpoint().port()) << endl;
        cout << "<D_IP>: " << request_endpoint_.address().to_string() << endl;
        cout << "<D_PORT>: " << to_string(request_endpoint_.port()) << endl;
        if (request_data_[1] == 1) {
            cout << "<Command>: CONNECT" << endl;
        } else {
            cout << "<Command>: BIND" << endl;
        }
        cout << "<Reply>: " << reply << endl;
    }

    void do_fail_reply() {
        output_request_result("Reject");
        do_write_socks4_reply(91, in_socket_.local_endpoint());
        do_close();
    }

    void do_read_socks4_request() {
        auto self(shared_from_this());
        in_socket_.async_read_some(boost::asio::buffer(request_data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    if (request_data_[0] != 4) {
                        return;
                    }
                    do_resolve();
                    if (firewall() == false) {
                        do_fail_reply();
                        return;
                    }

                    if (request_data_[1] == 1) {
                        do_connect();
                        if (out_socket_.native_handle() < 0) {
                            do_fail_reply();
                            return;
                        } else {
                            output_request_result("Accept");
                            do_write_socks4_reply(90, in_socket_.local_endpoint());
                        }
                    } else if (request_data_[1] == 2) {
                        do_bind();
                        if (out_socket_.native_handle() < 0) {
                            do_fail_reply();
                            return;
                        } else {
                            output_request_result("Accept");
                        }
                    }

                    in_socket_read();
                    out_socket_read();
                }
        });
    }

    void do_write_socks4_reply(int reply, tcp::endpoint endpoint) {
        unsigned short port = endpoint.port();
        unsigned int ip = endpoint.address().to_v4().to_ulong();
        char packet[8];
        packet[0] = 0;
        packet[1] = reply;
        packet[2] = port >> 8 & 0xFF;
        packet[3] = port & 0xFF;
        packet[4] = ip >> 24 & 0xFF;
        packet[5] = ip >> 16 & 0xFF;
        packet[6] = ip >> 8 & 0xFF;
        packet[7] = ip & 0xFF;

        boost::asio::write(in_socket_, boost::asio::buffer(packet, 8));
    }

    void in_socket_read() {
        auto self(shared_from_this());
        in_socket_.async_read_some(boost::asio::buffer(in_data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    out_socket_write(length);
                } else {
                    do_close();
                }
        });
    }

    void out_socket_read() {
        auto self(shared_from_this());
        out_socket_.async_read_some(boost::asio::buffer(out_data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    in_socket_write(length);
                } else {
                    do_close();
                }
        });
    }

    void in_socket_write(size_t length) {
        auto self(shared_from_this());
        boost::asio::async_write(in_socket_, boost::asio::buffer(out_data_, length),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    memset(out_data_, 0, max_length);
                    out_socket_read();
                } else {
                    do_close();
                }
        });
    }
    
    
    void out_socket_write(size_t length) {
        auto self(shared_from_this());
        boost::asio::async_write(out_socket_, boost::asio::buffer(in_data_, length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    memset(in_data_, 0, max_length);
                    in_socket_read();
                } else {
                    do_close();
                }
        });
    }

    tcp::socket in_socket_;
    tcp::socket out_socket_;
    tcp::endpoint request_endpoint_;
    enum { max_length = 1024 };
    char in_data_[max_length];
    char out_data_[max_length];
    char request_data_[max_length];
};

class server {
public:
    server(short port): acceptor_(io_service, tcp::endpoint(tcp::v4(), port)) {
            do_accept();
    }

private:
    static void ChildHandler(int signo) {
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {
        }
    }

    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                io_service.notify_fork(boost::asio::io_service::fork_prepare);
                signal(SIGCHLD, ChildHandler);
                pid_t pid = fork();
                while (pid < 0) {
                    int status;
                    waitpid(-1, &status, 0);
                    pid = fork();
                }
                
                if (pid == 0) {
                    io_service.notify_fork(boost::asio::io_service::fork_child);
                    acceptor_.close();
                    std::make_shared<session>(std::move(socket))->start();
                } else {
                    io_service.notify_fork(boost::asio::io_service::fork_parent);
                    socket.close();
                    do_accept();
                }
            }
        });
    }

    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            cerr << "Usage: async_tcp_echo_server <port>\n";
            return 1;
        }

        server serv(std::atoi(argv[1]));

        io_service.run();
    } catch (exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}