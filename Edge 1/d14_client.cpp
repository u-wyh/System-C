#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace std;

int main() {
    const string server_ip = "127.0.0.1";
    const int server_port = 8080;

    while (true) {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            cerr << "Socket creation failed: " << strerror(errno) << endl;
            this_thread::sleep_for(chrono::seconds(1));
            continue;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

        cout << "[Client] Connecting to " << server_ip << ":" << server_port << " ..." << endl;
        if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            cerr << "[Client] Connection failed: " << strerror(errno) << endl;
            close(sockfd);
            this_thread::sleep_for(chrono::seconds(1));
            continue;
        }

        cout << "[Client] Connected!" << endl;
        int counter = 1;
        try {
            while (true) {
                string msg = to_string(counter++);
                ssize_t sent = send(sockfd, msg.c_str(), msg.size(), 0);
                if (sent <= 0) {
                    cerr << "[Client] Send failed, reconnecting..." << endl;
                    break;
                }

                char buffer[1024];
                ssize_t n = recv(sockfd, buffer, sizeof(buffer)-1, 0);
                if (n <= 0) {
                    cerr << "[Client] Receive failed, reconnecting..." << endl;
                    break;
                }
                buffer[n] = '\0';
                cout << "[Server echo] " << buffer << endl;

                this_thread::sleep_for(chrono::seconds(1));
            }
        } catch (...) {
            cerr << "[Client] Exception occurred, reconnecting..." << endl;
        }

        close(sockfd);
        cout << "[Client] Disconnected, retrying in 1s..." << endl;
        this_thread::sleep_for(chrono::seconds(1));
    }

    return 0;
}
