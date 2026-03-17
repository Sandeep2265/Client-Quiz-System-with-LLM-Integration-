#include <iostream>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <chrono>

using namespace std;
using json = nlohmann::json;

#define PORT 11027
#define max_clients 10
#define KEEP_ALIVE_INTERVAL 90
#define KEEP_ALIVE_TIMEOUT 25
#define QUIZ_TOTAL_TIME 1200

struct Quiz {
    string question;
    vector<string> options;
    string answer;
};

struct Student {
    string name;
    int socket;
    chrono::time_point<chrono::steady_clock> lastAlive;
};

struct Cache_Quiz {
    vector<Quiz> questions;
    chrono::time_point<chrono::steady_clock> timestamp;
};

unordered_map<string, Cache_Quiz> quiz_from_cache;
mutex cache_mutex;
map<string, int> Scores;
mutex scores_mutex;

int CurlWriteCallback(void* Data, size_t Size, size_t Count, void* userPointer) {
    string* responseStorage = static_cast<string*>(userPointer);
    int no_of_Bytes = Size * Count;
    responseStorage->append(static_cast<char*>(Data), no_of_Bytes);
    return no_of_Bytes;
}

string httpPostRequest(const string& targetUrl, const string& jsonPayload) {
    CURL* curlHandle = curl_easy_init();
    string response;

    if (curlHandle) {
        struct curl_slist* headersList = nullptr;
        headersList = curl_slist_append(headersList, "Content-Type: application/json");

        curl_easy_setopt(curlHandle, CURLOPT_URL, targetUrl.c_str());
        curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headersList);
        curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, jsonPayload.c_str());
        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT, 60L);

        CURLcode res = curl_easy_perform(curlHandle);

        if (res != CURLE_OK) {
            cout << "CURL error: " << curl_easy_strerror(res) << endl;
        }
        curl_slist_free_all(headersList);
        curl_easy_cleanup(curlHandle);
    }

    return response;
}

bool reserve(const string& rollno) {
    json j;
    j["rollno"] = rollno;
    string resp = httpPostRequest("http://192.168.50.142:25000/reserve", j.dump());
    // cout << "[DEBUG] Reserve response: " << resp << endl;
    if (resp.empty())
        return false;
    try {
        json jresp = json::parse(resp);
        if (jresp.contains("status")) {
            string status = jresp["status"].get<string>();
            if (status == "success" || status == "already_reserved" || status == "reserved") {
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

vector<Quiz> generateQuiz(const string &rollno, const string &genre)
{
    vector<Quiz> questions;

    if (!reserve(rollno)) {
        cerr << "Reservation failed for " << rollno << endl;
        return questions;
    }

    json requestBody;
    requestBody["rollno"] = rollno;
    requestBody["messages"] = json::array({
        {
            {"role", "system"},
            {"content",
             "You are a trivia generator. Given a genre, output exactly 6 multiple-choice "
             "questions. Each question must have 4 options (A, B, C, D) and one correct answer. "
             "Return ONLY plain text in the following format:\n"
             "1. Question text\nA) Option A\nB) Option B\nC) Option C\nD) Option D\nAnswer: X\n ... for all 10 questions."}
        },
        {
            {"role", "user"},
            {"content", "Generate 6 MCQ trivia questions about " + genre + "."}}
    });
    requestBody["temperature"] = 0.0;
    requestBody["max_tokens"] = 1200;

    string resp = httpPostRequest("http://192.168.50.142:25000/generate_quiz", requestBody.dump());
   // cout << "Sending JSON to API: " << requestBody.dump() << endl;

    if (resp.empty()) {
        cerr << "Empty response from API.Quiz cannot be generated.\n";
        return questions; 
    }

    try {
        json jresp = json::parse(resp);
        //cout << "Raw API response:\n" << resp << endl;
        if (jresp.contains("choices") && jresp["choices"].size() > 0) {
            string assistantContent = jresp["choices"][0]["message"]["content"].get<string>();

            istringstream iss(assistantContent);
            string line;
            Quiz q;
            while (getline(iss, line)) {
                if (line.empty()) continue;

                if (isdigit(line[0]) && line.find('.') != string::npos) {
                    if (!q.question.empty()) questions.push_back(q);
                    q = Quiz{};
                    q.question = line.substr(line.find('.') + 1);
                    q.options.clear();
                    q.answer.clear();
                }
                
                else if (line.size() > 2 && line[0] >= 'A' && line[0] <= 'D' && line[1] == ')') {
                    q.options.push_back(line);
                }
            
                else if (line.rfind("Answer:", 0) == 0) {
                    q.answer = line.substr(8); 
                    q.answer.erase(remove_if(q.answer.begin(), q.answer.end(), ::isspace), q.answer.end());
                }
            }
            if (!q.question.empty()) {
                questions.push_back(q);
            }
        }
    } 
    catch (const std::exception &e) {
        cerr << "JSON parsing error: " << e.what() << endl;
    }

    return questions;

}


void give_leaderboard(int client_socket) {
    vector<pair<int, string>> allScores;
    for (auto it = Scores.begin(); it != Scores.end(); ++it) {
        allScores.push_back(make_pair(it->second, it->first));
    }
    string leaderboard = "Leaderboard:\n";
    for (auto it = allScores.rbegin(); it != allScores.rend(); ++it) {
    leaderboard += it->second + ": " + to_string(it->first) + "\n";
    }
    send(client_socket, leaderboard.c_str(), leaderboard.size(), 0);
}

void handle(Student student, vector<Quiz> quiz_questions) {
    int client_socket = student.socket;
    string client_name = student.name;
    unordered_map<string,string> qid_ans;
    {
        lock_guard<mutex> lock(scores_mutex);
        Scores[client_name] = 0;
    }
    cout << "Client " << client_name << " connected on socket " << client_socket << endl;
    auto quiz_start = chrono::steady_clock::now();
    auto lastKeepAlive = chrono::steady_clock::now();
    char buffer[4096];

    for (int i = 0; i < quiz_questions.size(); i++) {
        auto now = chrono::steady_clock::now();
        if (chrono::duration_cast<chrono::seconds>(now - quiz_start).count() >= QUIZ_TOTAL_TIME) {
            int totalScore;
            {
                lock_guard<mutex> lock(scores_mutex);
                totalScore = Scores[client_name];
            }
            string msg = string("Quiz over final scores available your final score is ") + to_string(totalScore) + "\n";
            send(client_socket, msg.c_str(), msg.size(), 0);
            close(client_socket);
            cout << "Quiz time completed for client " << client_name << endl;
            return;
        }

        string qid = "Q" + to_string(i+1);
        string question_line = "QUES " + qid + "\n" + quiz_questions[i].question + "\n";
        for (int j=0;j<quiz_questions[i].options.size();j++) {
            question_line += quiz_questions[i].options[j];
            if (j < quiz_questions[i].options.size()-1){
                question_line += "\n";
            }
        }
        question_line += "\n";

        send(client_socket, question_line.c_str(), question_line.size(), 0);
        qid_ans[qid] = quiz_questions[i].answer;

        auto lastKeepAlive = chrono::steady_clock::now();
        bool answered = false;
        bool keepAliveSent = false;

        while (!answered) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);

            auto now = chrono::steady_clock::now();
            int taken = chrono::duration_cast<chrono::seconds>(now - lastKeepAlive).count();
            int timeout = KEEP_ALIVE_INTERVAL - taken;
            if (timeout <= 0){ 
                timeout = 0;
            }

            struct timeval tv;
            tv.tv_sec = timeout;
            tv.tv_usec = 0;

            int activity = select(client_socket + 1, &readfds, NULL, NULL, &tv);
            if (activity > 0 && FD_ISSET(client_socket, &readfds)) {
                memset(buffer,0,sizeof(buffer));
                int bytes = read(client_socket, buffer, sizeof(buffer));
                if (bytes <= 0) {
                    cout << "Client " << client_name << " disconnected." << endl;
                    close(client_socket);
                    return;
                }
                string response(buffer);
                if (response.find("ALIVE_OK") != string::npos) {
                    lastKeepAlive = chrono::steady_clock::now();
                    keepAliveSent = false;
                    continue;
                }

                char client_option = toupper(response[0]);
                string result;
                if (client_option == qid_ans[qid][0] && (client_option == 'A' || client_option == 'B' || client_option == 'C' || client_option == 'D')) {
                    result = "Correct!\n";
                    lock_guard<mutex> lock(scores_mutex);
                    Scores[client_name]++;
                } 
                else {
                    result = "Wrong!\n";
                }

                send(client_socket, result.c_str(), result.size(), 0);
                

                string ask = "Do you want Question or do you want to see the Leaderboard\n";
                send(client_socket, ask.c_str(), ask.size(), 0);

                memset(buffer,0,sizeof(buffer));
                int bytes2 = read(client_socket, buffer, sizeof(buffer));
                if (bytes2 > 0) {
                    string choice(buffer);
                    if (choice.find("Leaderboard") != string::npos) {
                        give_leaderboard(client_socket);
                    }
                }
                answered=true;
                lastKeepAlive = chrono::steady_clock::now();
                if (i==quiz_questions.size()-1){
                    int totalScore;
                    {
                        lock_guard<mutex> lock(scores_mutex);
                        totalScore = Scores[client_name];
                    }
                    string msg = string("Quiz over  final scores available your final score is ") + to_string(totalScore) + "\n";
                    send(client_socket, msg.c_str(), msg.size(), 0);
                    close(client_socket);
                    cout << "Client " << client_name << " finished quiz." << endl;
                    return;
                }
               
            } 
            else{
                 if (!keepAliveSent) {
                    string keepMsg = "KEEP_ALIVE\n";
                    send(client_socket, keepMsg.c_str(), keepMsg.size(), 0);
                    keepAliveSent = true;
                    lastKeepAlive = chrono::steady_clock::now();
                } else {
                    auto now2 = chrono::steady_clock::now();
                    if (chrono::duration_cast<chrono::seconds>(now2 - lastKeepAlive).count() >= KEEP_ALIVE_TIMEOUT) {
                        cout << "Client " << client_name << " did not respond to KEEP_ALIVE.Terminating quiz.\n";
                        close(client_socket);
                        return;
                    }
                }
            }
        }
    }
    int totalScore;
     {
       lock_guard<mutex> lock(scores_mutex);
       totalScore = Scores[client_name];
     }
    string msg = string("Quiz over final scores available your final score is ") + to_string(totalScore) + "\n";
    send(client_socket, msg.c_str(), msg.size(), 0);
    close(client_socket);
    cout << "Client " << client_name << " finished quiz." << endl;
}

vector<Quiz> get_quiz_from_cache(const string &rollno, const string &genre,int client_sock) {
    lock_guard<mutex> lock(cache_mutex);

    auto it = quiz_from_cache.find(genre);
    if (it != quiz_from_cache.end()) {
        auto now = chrono::steady_clock::now();
        auto age = chrono::duration_cast<chrono::seconds>(now - it->second.timestamp).count();

        if (age < 40) {
            cout << "CACHE HIT Serving quiz from cache for genre " << genre << endl;
            string msg = "CACHE HIT Serving quiz from cache for genre " + genre + "\n";
            send(client_sock, msg.c_str(), msg.size(), 0);
            return it->second.questions;
        } else {
            cout << "CACHE EXPIRED Regenerating quiz for genre as it stayed long time " << genre << endl;
             string msg = "CACHE EXPIRED Regenerating quiz for genre " + genre + "\n";
            send(client_sock, msg.c_str(), msg.size(), 0);
            quiz_from_cache.erase(it);
        }
    }

    cout << "CACHE MISS Generating new quiz for genre " << genre << endl;
    string msg = "CACHE MISS Generating new quiz for genre " + genre + "\n";
    send(client_sock, msg.c_str(), msg.size(), 0);
    vector<Quiz> new_quiz = generateQuiz(rollno, genre);

    Cache_Quiz entry;
    entry.questions = new_quiz;
    entry.timestamp = chrono::steady_clock::now();
    quiz_from_cache[genre] = entry;

    return new_quiz;
}


int main() {
    int server_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0){
         return 1;
        }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr*)&address, sizeof(address)) < 0){ 
        return 1;
    }
    if (listen(server_socket, max_clients) < 0) {
        return 1;
    }
    cout << "Server listening on port " << PORT << "\n";

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addrlen);
        if (client_socket < 0) {
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);

        char buffer[4096];
        memset(buffer,0,sizeof(buffer));
        read(client_socket, buffer, sizeof(buffer));
        string client_message(buffer);
        int pos = client_message.find('|');
        string client_name = client_message.substr(0,pos);
        string genre = client_message.substr(pos+1);
        cout << "Client " << client_ip << ":" << client_port << " connected (name: " << client_name << ")" << endl;

        string rollno = "cs23btech11027";

        vector<Quiz> quiz_questions = get_quiz_from_cache(rollno, genre,client_socket);
        cout << "Quiz questions generated: " << quiz_questions.size() << endl;

        Student student{client_name, client_socket, chrono::steady_clock::now()};
        thread t(handle, student, quiz_questions);
        t.detach();
    }

    close(server_socket);
    return 0;
}

