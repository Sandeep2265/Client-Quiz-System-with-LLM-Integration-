#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <mutex>

#define SERVER_IP "192.168.50.111"
#define PORT 11027

using namespace std;

vector<string> genres = {"Science", "History", "Sports", "Math", "Geography"};
mutex cout_mutex;

void autoClient() {
    int num_clients;
    cout << "Enter number of automatic clients: ";
    cin >> num_clients;

    vector<thread> threads;

    for (int i = 0; i < num_clients; i++) {
        string client_name = "USER" + to_string(i + 1);

        threads.push_back(thread([client_name]() {
            srand(time(0) + client_name.length());
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0){
                 return;
            }

            struct sockaddr_in serv_addr{};
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(PORT);
            inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);

            if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                return;
            }
            string genre = genres[rand() % genres.size()];
            string message = client_name + "|" + genre;
            send(sock, message.c_str(), message.size(), 0);

            {
                lock_guard<mutex> lock(cout_mutex);
                cout << client_name << " selected genre: " << genre << endl;
            }

            char buffer[4096];
            bool quiz_over = false;

            while (!quiz_over) {
                memset(buffer, 0, sizeof(buffer));
                int valread = read(sock, buffer, sizeof(buffer));
                if (valread <= 0) {
                    break;
                }
                string server_msg(buffer);

                {
                    lock_guard<mutex> lock(cout_mutex);
                    cout << client_name << " RECEIVED: " << server_msg << endl;
                }

                if (server_msg.find("Do you want Question or do you want to see the Leaderboard") != string::npos) {
                    string choice = "Question";
                    send(sock, choice.c_str(), choice.size(), 0);
                }
                else if (server_msg.find("KEEP_ALIVE") != string::npos) {
                    string alive = "ALIVE_OK";
                    send(sock, alive.c_str(), alive.size(), 0);
                }
                else if (server_msg.find("QUES") != string::npos || server_msg.find("QUESTION") != string::npos) {
                    char opt = 'A' + rand() % 4; // random option
                    string answer(1, opt);
                    send(sock, answer.c_str(), answer.size(), 0);

                    {
                        lock_guard<mutex> lock(cout_mutex);
                        cout << client_name << " Answered: " << answer << endl;
                    }
                }
                else if (server_msg.find("Quiz over") != string::npos) {
                    quiz_over = true;
                }
            }

            {
                lock_guard<mutex> lock(cout_mutex);
                cout << client_name << " disconnected." << endl << endl;
            }

            close(sock);
        }));
    }

    for (int i = 0; i < threads.size(); i++) {
        threads[i].join();
    }
}

void manualClient() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cout << "Socket creation error\n";
        return;
    }

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        cout << "Invalid address \n";
        return;
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        cout << "Connection Failed\n";
        return;
    }

    string client_name, genre;
    cout << "Enter your name: ";
    cin >> client_name;
    cout << "Enter genre (e.g., Science, History, Sports): ";
    cin >> genre;

    string message = client_name + "|" + genre;
    send(sock, message.c_str(), message.size(), 0);

    char buffer[4096];

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int valread = read(sock, buffer, sizeof(buffer));

        if (valread <= 0) {
            cout << "Server disconnected.\n";
            break;
        }

        string server_msg(buffer);
        cout << server_msg;

        if (server_msg.find("QUES") != string::npos) {
            string answer;
            cout << "Select one option: ";
            cin >> answer;
            send(sock, answer.c_str(), answer.size(), 0);
        }
        else if (server_msg.find("Do you want Question or do you want to see the Leaderboard") != string::npos) {
            int choice;
            cout << "(Type 1 for Question, 2 for Leaderboard): ";
            cin >> choice;
            string temp;
            if (choice == 1) {
                temp = "Question";
            } else if (choice == 2) {
                temp = "Leaderboard";
            }
            send(sock, temp.c_str(), temp.size(), 0);
        }
        else if (server_msg.find("KEEP_ALIVE") != string::npos) {
            string choice;
            cout << "Server sent KEEP_ALIVE. Type 'ALIVE_OK': ";
            cin >> choice;
            send(sock, choice.c_str(), choice.size(), 0);
        }
    }

    close(sock);
}

int main() {
    srand(time(0));

    cout << "Choose mode (manual/auto): ";
    string mode;
    cin >> mode;

    if (mode == "manual") {
        manualClient();
    } else if (mode == "auto") {
        autoClient();
    } else {
        cout << "Invalid mode selected.\n";
    }

    return 0;
}

