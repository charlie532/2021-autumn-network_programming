#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <vector>

using namespace std;
using boost::asio::ip::tcp;
boost::asio::io_service io_service;

const int N_SERVERS = 5;

struct server_info {
    string hostname;
    string element_id;
    string port;
    string filename;
    ifstream fin;
    bool exist = false;
};

void print_html(server_info s[]) {
    cout << "<!DOCTYPE html>" << endl;
    cout << "<html lang=\"en\">" << endl;
    cout << "<head>" << endl;
    cout << "<meta charset=\"UTF-8\" />" << endl;
    cout << "<title>NP Project 3 Sample Console</title>" << endl;
    cout << "<link" << endl;
    cout << "rel=\"stylesheet\"" << endl;
    cout << "href=\"https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css\"" << endl;
    cout << "integrity=\"sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2\"" << endl;
    cout << "crossorigin=\"anonymous\"" << endl;
    cout << "/>" << endl;
    cout << "<link" << endl;
    cout << "href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\"" << endl;
    cout << "rel=\"stylesheet\"" << endl;
    cout << "/>" << endl;
    cout << "<link" << endl;
    cout << "rel=\"icon\"" << endl;
    cout << "type=\"image/png\"" << endl;
    cout << "href=\"https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png\"" << endl;
    cout << "/>" << endl;
    cout << "<style>" << endl;
    cout << "* {" << endl;
    cout << "font-family: 'Source Code Pro', monospace;" << endl;
    cout << "font-size: 1rem !important;" << endl;
    cout << "}" << endl;
    cout << "body {" << endl;
    cout << "background-color: #212529;" << endl;
    cout << "}" << endl;
    cout << "pre {" << endl;
    cout << "color: #cccccc;" << endl;
    cout << "}" << endl;
    cout << "b {" << endl;
    cout << "color: #01b468;" << endl;
    cout << "}" << endl;
    cout << "</style>" << endl;
    cout << "</head>" << endl;
    cout << "<body>" << endl;
    cout << "<table class=\"table table-dark table-bordered\">" << endl;

    cout << "<thead>" << endl;
    cout << "<tr>" << endl;
    for (int i = 0; i < N_SERVERS; ++i) {
        if (s[i].exist) {
            cout << "<th scope=\"col\">" << s[i].hostname << ":" << s[i].port << "</th>" << endl;
        }
    }
    cout << "</tr>" << endl;
    cout << "</thead>" << endl;

    cout << "<tbody>" << endl;
    cout << "<tr>" << endl;
    for (int i = 0; i < N_SERVERS; ++i) {
        if (s[i].exist) {
            cout << "<td><pre id=\"" << s[i].element_id << "\" class=\"mb-0\"></pre></td>" << endl;
        }
    }
    cout << "</tr>" << endl;
    cout << "</tbody>" << endl;

    cout << "</table>" << endl;
    cout << "</body>" << endl;
    cout << "</html>" << endl;
}

void parse_query(server_info s[]) {
    string query_string = string(getenv("QUERY_STRING"));
    vector<string> paras;
    boost::split(paras, query_string, boost::is_any_of("&"));

    for (int i = 0; i < (int)paras.size(); ++i) {
        if (paras[i].back() == '=') {
            continue;
        }
        int index = paras[i].find("=");
        int n = stoi(paras[i].substr(1, index - 1));

        if (paras[i][0] == 'h') {
            s[n].hostname = paras[i].substr(index + 1);
        } else if (paras[i][0] == 'p') {
            s[n].port = paras[i].substr(index + 1);
        } else if (paras[i][0] == 'f') {
            s[n].filename = "./test_case/" + paras[i].substr(index + 1);
        }
    }

    for (int i = 0; i < N_SERVERS; ++i) {
        if (s[i].hostname != "" && s[i].port != "" && s[i].filename != "") {
            s[i].element_id = "s" + to_string(i + 1);
            s[i].fin.open(s[i].filename, ifstream::in);
            s[i].exist = true;
        }
    }
}

class client {
public:
    client(): socket_(io_service), resolver_(io_service) {
        memset(data_, '\0', max_length);
    }

    void start(server_info &server) {
        do_resolve(server);
    }
private:

    void replace_string(string &data) {
        boost::replace_all(data, "<", "&lt;");
        boost::replace_all(data, ">", "&gt;");
        boost::replace_all(data, " ", "&nbsp;");
        boost::replace_all(data, "\"", "&quot;");
        boost::replace_all(data, "\'", "&apos;");
        boost::replace_all(data, "\r", "");
        boost::replace_all(data, "\n", "&NewLine;");
        boost::replace_all(data, "<", "&lt;");
    }

    void do_write(server_info &server, const char *buffer, size_t length) {
        boost::asio::async_write(socket_, boost::asio::buffer(buffer, length),
            [&, this, buffer](boost::system::error_code ec, size_t write_length) {
                if (!ec) {
                } else {
                    cerr << "Write => " << ec << endl;
                }
            });
    }

    void do_read(server_info &server) {
        socket_.async_read_some(
            boost::asio::buffer(data_, max_length),
            [this, &server](boost::system::error_code ec, size_t length) {
                if (!ec) {
                    string output = string(data_);
                    replace_string(output);
                    cout << "<script>document.getElementById('" << server.element_id << "').innerHTML += '" << output << "';</script>" << endl;
                    
                    if (strstr(data_, "% ")) {
                        output = "";

                        getline(server.fin, output);

                        if (output == "exit") {
                            server.fin.close();
                            server.exist = false;
                        }
                        boost::replace_all(output, "\r", "");
                        output.append("\n");
                        do_write(server, output.c_str(), output.size());

                        replace_string(output);
                        cout << "<script>document.getElementById('" << server.element_id << "').innerHTML += '<b>" << output << "</b>';</script>" << endl;
                    }

                    memset(data_, 0, max_length);
                    do_read(server);
                } else if (ec != boost::asio::error::eof) {
                    cerr << "Read => " << ec << endl;
                }
        });
    }
    
    void do_connect(server_info &server, tcp::resolver::iterator &it) {
        tcp::endpoint endpoint = it->endpoint();
        socket_.async_connect(endpoint, [this, &server](boost::system::error_code ec) {
            if (!ec) {
                do_read(server);
            } else {
                cerr << "Connect => " << ec << endl << endl;
            }
        });
    }

    void do_resolve(server_info &server) {
        tcp::resolver::query query(server.hostname, server.port);
        resolver_.async_resolve(query, [this, &server](boost::system::error_code ec, tcp::resolver::iterator it) {
            if (!ec) {
                do_connect(server, it);
            } else {
                cerr << "Resolve => " << ec << endl;
            }
        });
    }
    tcp::socket socket_;
    tcp::resolver resolver_;
    enum {max_length = 1024};
    char data_[max_length];
};

int main() {
    cout << "Content-type: text/html" << endl << endl;

    server_info serv[N_SERVERS];
    client cli[N_SERVERS];

    parse_query(serv);
    print_html(serv);
    
    for (int i = 0; i < N_SERVERS; ++i) {
        if (serv[i].exist) {
            cli[i].start(serv[i]);
        }
    }

    io_service.run();

    return 0;
}