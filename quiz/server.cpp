#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <regex>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <curl/curl.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================
// CONFIGURATION
// ============================================================

#define PORT 11027
#define MAX_CLIENTS 10

#define KEEP_ALIVE_INTERVAL 90
#define KEEP_ALIVE_TIMEOUT 25

#define QUIZ_TOTAL_TIME 1200

#define CACHE_EXPIRY_SECONDS 40

#define CACHE_DIRECTORY "quiz_cache"

// Local Groq replacement API
const string LLM_BASE_URL = "http://127.0.0.1:9000";

const string DB_FILE = "quiz.db";

// ============================================================
// DATA STRUCTURES
// ============================================================

struct Quiz {
    string question;
    vector<string> options;
    string answer;
};

struct Student {
    string gmail;
    string name;
    string genre;
    int questionCount;
    int socket;
};

struct CacheQuiz {
    vector<Quiz> questions;

    // Used for file-based cache expiry.
    long long createdAt;

    // Used for LRU-style access tracking.
    long long lastUsed;
};

// ============================================================
// GLOBAL DATA
// ============================================================

mutex cache_mutex;
mutex scores_mutex;
mutex db_mutex;

// Temporary score map for currently running quizzes.
map<string, int> Scores;

// ============================================================
// SQLITE DATABASE
// ============================================================

sqlite3* database = nullptr;

// ============================================================
// UTILITY FUNCTIONS
// ============================================================

long long currentUnixTime()
{
    return chrono::duration_cast<chrono::seconds>(
        chrono::system_clock::now().time_since_epoch()
    ).count();
}

string trim(const string& input)
{
    size_t start = input.find_first_not_of(" \t\r\n");

    if (start == string::npos)
        return "";

    size_t end = input.find_last_not_of(" \t\r\n");

    return input.substr(start, end - start + 1);
}

string toLower(string value)
{
    transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(tolower(c));
        }
    );

    return value;
}

bool validGmail(const string& gmail)
{
    static const regex pattern(
        R"(^[A-Za-z0-9._%+\-]+@gmail\.com$)",
        regex_constants::icase
    );

    return regex_match(gmail, pattern);
}

bool validQuestionCount(int count)
{
    return count == 5 || count == 10 || count == 15;
}

// ============================================================
// CURL
// ============================================================

size_t CurlWriteCallback(
    void* data,
    size_t size,
    size_t count,
    void* userPointer
)
{
    string* responseStorage =
        static_cast<string*>(userPointer);

    size_t bytes = size * count;

    responseStorage->append(
        static_cast<char*>(data),
        bytes
    );

    return bytes;
}

string httpPostRequest(
    const string& targetUrl,
    const string& jsonPayload
)
{
    CURL* curlHandle = curl_easy_init();

    string response;

    if (!curlHandle)
    {
        cerr << "Could not initialize CURL.\n";
        return "";
    }

    struct curl_slist* headersList = nullptr;

    headersList = curl_slist_append(
        headersList,
        "Content-Type: application/json"
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_URL,
        targetUrl.c_str()
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_HTTPHEADER,
        headersList
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_POSTFIELDS,
        jsonPayload.c_str()
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_WRITEFUNCTION,
        CurlWriteCallback
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_WRITEDATA,
        &response
    );

    curl_easy_setopt(
        curlHandle,
        CURLOPT_TIMEOUT,
        60L
    );

    CURLcode result =
        curl_easy_perform(curlHandle);

    if (result != CURLE_OK)
    {
        cerr << "CURL error: "
             << curl_easy_strerror(result)
             << endl;
    }

    curl_slist_free_all(headersList);

    curl_easy_cleanup(curlHandle);

    return response;
}

// ============================================================
// PROFESSOR / GROQ RESERVATION
// ============================================================

bool reserve(const string& rollno)
{
    json request;

    request["rollno"] = rollno;

    string response =
        httpPostRequest(
            LLM_BASE_URL + "/reserve",
            request.dump()
        );

    if (response.empty())
    {
        return false;
    }

    try
    {
        json result =
            json::parse(response);

        if (result.contains("status"))
        {
            string status =
                result["status"].get<string>();

            if (
                status == "success" ||
                status == "already_reserved" ||
                status == "reserved"
            )
            {
                return true;
            }
        }
    }
    catch (...)
    {
        return false;
    }

    return false;
}

// ============================================================
// QUIZ GENERATION
// ============================================================

vector<Quiz> generateQuiz(
    const string& rollno,
    const string& genre,
    int requestedCount
)
{
    vector<Quiz> questions;

    if (!reserve(rollno))
    {
        cerr
            << "Reservation failed for "
            << rollno
            << endl;

        return questions;
    }

    json requestBody;

    string systemPrompt =
        "You are a trivia generator. "
        "Generate exactly "
        + to_string(requestedCount)
        + " multiple-choice questions about the requested genre. "
          "Every question must have exactly four options: A, B, C and D. "
          "There must be exactly one correct answer. "
          "Return ONLY plain text. "
          "Do not use Markdown. "
          "Do not use code fences. "
          "Use exactly this format:\n\n"
          "1. Question text\n"
          "A) Option A\n"
          "B) Option B\n"
          "C) Option C\n"
          "D) Option D\n"
          "Answer: A\n\n"
          "2. Question text\n"
          "A) Option A\n"
          "B) Option B\n"
          "C) Option C\n"
          "D) Option D\n"
          "Answer: B\n\n"
          "Continue until exactly "
        + to_string(requestedCount)
        + " questions have been generated.";

    requestBody["rollno"] = rollno;

    requestBody["messages"] = json::array({
        {
            {
                "role",
                "system"
            },
            {
                "content",
                systemPrompt
            }
        },
        {
            {
                "role",
                "user"
            },
            {
                "content",
                "Generate exactly "
                + to_string(requestedCount)
                + " MCQ questions about "
                + genre
                + "."
            }
        }
    });

    requestBody["temperature"] = 0.0;

    requestBody["max_tokens"] = 4096;

    cout
        << "Generating "
        << requestedCount
        << " questions for genre: "
        << genre
        << " ...\n";

    string response =
        httpPostRequest(
            LLM_BASE_URL + "/generate_quiz",
            requestBody.dump()
        );

    if (response.empty())
    {
        cerr
            << "Empty response from LLM API.\n";

        return questions;
    }

    try
    {
        json result =
            json::parse(response);

        if (
            !result.contains("choices") ||
            result["choices"].empty()
        )
        {
            cerr
                << "LLM response contains no choices.\n";

            return questions;
        }

        string content =
            result["choices"][0]
                   ["message"]
                   ["content"]
                   .get<string>();

        istringstream input(content);

        string line;

        Quiz currentQuiz;

        bool insideQuestion = false;

        while (getline(input, line))
        {
            line = trim(line);

            if (line.empty())
            {
                continue;
            }

            // Remove Markdown code fences if the model
            // accidentally produces them.
            if (
                line == "```" ||
                line == "```text" ||
                line == "```plaintext"
            )
            {
                continue;
            }

            // ------------------------------------------------
            // Question
            // Supports:
            //
            // 1. Question
            // 1) Question
            // Q1. Question
            // Q1) Question
            // ------------------------------------------------

            smatch questionMatch;

            regex questionPattern(
                R"(^Q?([0-9]+)[\.\)]\s*(.+)$)",
                regex_constants::icase
            );

            if (
                regex_match(
                    line,
                    questionMatch,
                    questionPattern
                )
            )
            {
                if (insideQuestion &&
                    !currentQuiz.question.empty())
                {
                    if (
                        currentQuiz.options.size() == 4 &&
                        !currentQuiz.answer.empty()
                    )
                    {
                        questions.push_back(
                            currentQuiz
                        );
                    }
                }

                currentQuiz = Quiz{};

                currentQuiz.question =
                    trim(
                        questionMatch[2].str()
                    );

                insideQuestion = true;

                continue;
            }

            if (!insideQuestion)
            {
                continue;
            }

            // ------------------------------------------------
            // Options
            // ------------------------------------------------

            regex optionPattern(
                R"(^([A-D])[\)\.:]\s*(.+)$)",
                regex_constants::icase
            );

            smatch optionMatch;

            if (
                regex_match(
                    line,
                    optionMatch,
                    optionPattern
                )
            )
            {
                string optionLetter =
                    optionMatch[1].str();

                string optionText =
                    optionMatch[2].str();

                optionLetter =
                    string(
                        1,
                        static_cast<char>(
                            toupper(
                                static_cast<unsigned char>(
                                    optionLetter[0]
                                )
                            )
                        )
                    );

                currentQuiz.options.push_back(
                    optionLetter +
                    ") " +
                    trim(optionText)
                );

                continue;
            }

            // ------------------------------------------------
            // Answer
            // ------------------------------------------------

            string lowerLine =
                toLower(line);

            if (
                lowerLine.rfind(
                    "answer:",
                    0
                ) == 0
            )
            {
                string answer =
                    trim(
                        line.substr(
                            line.find(':') + 1
                        )
                    );

                if (!answer.empty())
                {
                    currentQuiz.answer =
                        string(
                            1,
                            static_cast<char>(
                                toupper(
                                    static_cast<unsigned char>(
                                        answer[0]
                                    )
                                )
                            )
                        );
                }

                continue;
            }

            if (
                lowerLine.rfind(
                    "correct answer:",
                    0
                ) == 0
            )
            {
                string answer =
                    trim(
                        line.substr(
                            line.find(':') + 1
                        )
                    );

                if (!answer.empty())
                {
                    currentQuiz.answer =
                        string(
                            1,
                            static_cast<char>(
                                toupper(
                                    static_cast<unsigned char>(
                                        answer[0]
                                    )
                                )
                            )
                        );
                }

                continue;
            }
        }

        // Add final question.
        if (
            insideQuestion &&
            !currentQuiz.question.empty()
        )
        {
            if (
                currentQuiz.options.size() == 4 &&
                !currentQuiz.answer.empty()
            )
            {
                questions.push_back(
                    currentQuiz
                );
            }
        }
    }
    catch (const exception& e)
    {
        cerr
            << "JSON parsing error: "
            << e.what()
            << endl;
    }

    // Never return more than requested.
    if (
        static_cast<int>(questions.size())
        > requestedCount
    )
    {
        questions.resize(
            requestedCount
        );
    }

    cout
        << "Generated "
        << questions.size()
        << " / "
        << requestedCount
        << " questions.\n";

    return questions;
}

// ============================================================
// SQLITE INITIALIZATION
// ============================================================

bool initializeDatabase()
{
    int result =
        sqlite3_open(
            DB_FILE.c_str(),
            &database
        );

    if (result != SQLITE_OK)
    {
        cerr
            << "Cannot open SQLite database: "
            << sqlite3_errmsg(database)
            << endl;

        return false;
    }

    const char* usersTable = R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            gmail TEXT NOT NULL COLLATE NOCASE UNIQUE,
            username TEXT NOT NULL COLLATE NOCASE UNIQUE,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )SQL";

    const char* resultsTable = R"SQL(
        CREATE TABLE IF NOT EXISTS quiz_results (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER NOT NULL,
            genre TEXT NOT NULL COLLATE NOCASE,
            question_count INTEGER NOT NULL,
            score INTEGER NOT NULL,
            played_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )SQL";

    char* errorMessage = nullptr;

    {
        lock_guard<mutex> lock(db_mutex);

        result =
            sqlite3_exec(
                database,
                usersTable,
                nullptr,
                nullptr,
                &errorMessage
            );

        if (result != SQLITE_OK)
        {
            cerr
                << "Users table error: "
                << (errorMessage ? errorMessage : "")
                << endl;

            sqlite3_free(errorMessage);

            return false;
        }

        result =
            sqlite3_exec(
                database,
                resultsTable,
                nullptr,
                nullptr,
                &errorMessage
            );

        if (result != SQLITE_OK)
        {
            cerr
                << "Results table error: "
                << (errorMessage ? errorMessage : "")
                << endl;

            sqlite3_free(errorMessage);

            return false;
        }

        const char* indexSQL = R"SQL(
            CREATE INDEX IF NOT EXISTS idx_leaderboard
            ON quiz_results(
                genre,
                question_count,
                score
            );
        )SQL";

        result =
            sqlite3_exec(
                database,
                indexSQL,
                nullptr,
                nullptr,
                &errorMessage
            );

        if (result != SQLITE_OK)
        {
            cerr
                << "Index creation error: "
                << (errorMessage ? errorMessage : "")
                << endl;

            sqlite3_free(errorMessage);

            return false;
        }
    }

    cout
        << "SQLite database ready: "
        << DB_FILE
        << endl;

    return true;
}

// ============================================================
// USER LOOKUP
// ============================================================

int getUserIdByGmail(
    const string& gmail
)
{
    const char* sql =
        "SELECT id FROM users "
        "WHERE gmail = ? COLLATE NOCASE;";

    sqlite3_stmt* statement = nullptr;

    lock_guard<mutex> lock(db_mutex);

    if (
        sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK
    )
    {
        return -1;
    }

    sqlite3_bind_text(
        statement,
        1,
        gmail.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    int userId = -1;

    if (
        sqlite3_step(statement)
        == SQLITE_ROW
    )
    {
        userId =
            sqlite3_column_int(
                statement,
                0
            );
    }

    sqlite3_finalize(statement);

    return userId;
}

int getUserIdByUsername(
    const string& username
)
{
    const char* sql =
        "SELECT id FROM users "
        "WHERE username = ? COLLATE NOCASE;";

    sqlite3_stmt* statement = nullptr;

    lock_guard<mutex> lock(db_mutex);

    if (
        sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK
    )
    {
        return -1;
    }

    sqlite3_bind_text(
        statement,
        1,
        username.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    int userId = -1;

    if (
        sqlite3_step(statement)
        == SQLITE_ROW
    )
    {
        userId =
            sqlite3_column_int(
                statement,
                0
            );
    }

    sqlite3_finalize(statement);

    return userId;
}

// ============================================================
// USER REGISTRATION
// ============================================================

bool registerOrLoginUser(
    const string& gmail,
    const string& username,
    int& userId,
    string& error
)
{
    int gmailUserId =
        getUserIdByGmail(gmail);

    int usernameUserId =
        getUserIdByUsername(username);

    // --------------------------------------------------------
    // New Gmail + new username
    // --------------------------------------------------------

    if (
        gmailUserId == -1 &&
        usernameUserId == -1
    )
    {
        const char* sql =
            "INSERT INTO users(gmail, username) "
            "VALUES(?, ?);";

        sqlite3_stmt* statement =
            nullptr;

        lock_guard<mutex> lock(db_mutex);

        if (
            sqlite3_prepare_v2(
                database,
                sql,
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK
        )
        {
            error =
                "Could not prepare user registration.";

            return false;
        }

        sqlite3_bind_text(
            statement,
            1,
            gmail.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        sqlite3_bind_text(
            statement,
            2,
            username.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        int result =
            sqlite3_step(statement);

        sqlite3_finalize(statement);

        if (result != SQLITE_DONE)
        {
            error =
                "Could not register user.";

            return false;
        }

        userId =
            static_cast<int>(
                sqlite3_last_insert_rowid(
                    database
                )
            );

        return true;
    }

    // --------------------------------------------------------
    // Existing Gmail + same username
    // --------------------------------------------------------

    if (
        gmailUserId != -1 &&
        usernameUserId != -1 &&
        gmailUserId == usernameUserId
    )
    {
        userId = gmailUserId;

        return true;
    }

    // --------------------------------------------------------
    // Existing Gmail + different username
    // --------------------------------------------------------

    if (
        gmailUserId != -1 &&
        usernameUserId != gmailUserId
    )
    {
        error =
            "This Gmail ID is already registered "
            "with another username.";

        return false;
    }

    // --------------------------------------------------------
    // New Gmail + username already used
    // --------------------------------------------------------

    if (
        gmailUserId == -1 &&
        usernameUserId != -1
    )
    {
        error =
            "Username is already registered. "
            "Please choose another username.";

        return false;
    }

    error =
        "Unable to register this account.";

    return false;
}

// ============================================================
// SAVE RESULT
// ============================================================

bool saveQuizResult(
    int userId,
    const string& genre,
    int questionCount,
    int score
)
{
    const char* sql =
        "INSERT INTO quiz_results("
        "user_id, genre, question_count, score"
        ") VALUES(?, ?, ?, ?);";

    sqlite3_stmt* statement =
        nullptr;

    lock_guard<mutex> lock(db_mutex);

    if (
        sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK
    )
    {
        return false;
    }

    sqlite3_bind_int(
        statement,
        1,
        userId
    );

    sqlite3_bind_text(
        statement,
        2,
        genre.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    sqlite3_bind_int(
        statement,
        3,
        questionCount
    );

    sqlite3_bind_int(
        statement,
        4,
        score
    );

    int result =
        sqlite3_step(statement);

    sqlite3_finalize(statement);

    return result == SQLITE_DONE;
}

// ============================================================
// LEADERBOARD
// ============================================================

string buildLeaderboard(
    const string& genre,
    int questionCount
)
{
    const char* sql = R"SQL(
        SELECT u.username, MAX(r.score) AS best_score
        FROM quiz_results r
        JOIN users u
            ON u.id = r.user_id
        WHERE r.genre = ?
          AND r.question_count = ?
        GROUP BY u.id
        ORDER BY best_score DESC, u.username ASC
        LIMIT 10;
    )SQL";

    sqlite3_stmt* statement =
        nullptr;

    lock_guard<mutex> lock(db_mutex);

    string leaderboard =
        "Leaderboard:\n"
        "Genre: " + genre + "\n" +
        "Questions: " +
        to_string(questionCount) +
        "\n"
        "--------------------\n";

    if (
        sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement,
            nullptr
        ) != SQLITE_OK
    )
    {
        return leaderboard;
    }

    sqlite3_bind_text(
        statement,
        1,
        genre.c_str(),
        -1,
        SQLITE_TRANSIENT
    );

    sqlite3_bind_int(
        statement,
        2,
        questionCount
    );

    int rank = 1;

    while (
        sqlite3_step(statement)
        == SQLITE_ROW
    )
    {
        const unsigned char* username =
            sqlite3_column_text(
                statement,
                0
            );

        int score =
            sqlite3_column_int(
                statement,
                1
            );

        leaderboard +=
            to_string(rank) +
            ". " +
            string(
                reinterpret_cast<const char*>(
                    username
                )
            ) +
            ": " +
            to_string(score) +
            "/" +
            to_string(questionCount) +
            "\n";

        rank++;
    }

    sqlite3_finalize(statement);

    if (rank == 1)
    {
        leaderboard +=
            "No scores yet.\n";
    }

    return leaderboard;
}

void give_leaderboard(
    int clientSocket,
    const string& genre,
    int questionCount
)
{
    string leaderboard =
        buildLeaderboard(
            genre,
            questionCount
        );

    send(
        clientSocket,
        leaderboard.c_str(),
        leaderboard.size(),
        0
    );
}

// ============================================================
// FILE-BASED CACHE
// ============================================================

string cacheKey(
    const string& genre,
    int questionCount
)
{
    string key =
        toLower(trim(genre));

    for (char& c : key)
    {
        if (
            !isalnum(
                static_cast<unsigned char>(c)
            )
        )
        {
            c = '_';
        }
    }

    return key +
           "_" +
           to_string(questionCount);
}

string cacheFilePath(
    const string& genre,
    int questionCount
)
{
    return string(CACHE_DIRECTORY) +
           "/" +
           cacheKey(
               genre,
               questionCount
           ) +
           ".cache";
}

bool saveQuizToCache(
    const string& genre,
    int questionCount,
    const vector<Quiz>& questions
)
{
    lock_guard<mutex> lock(cache_mutex);

    try
    {
        fs::create_directories(
            CACHE_DIRECTORY
        );
    }
    catch (...)
    {
        cerr
            << "Could not create cache directory.\n";

        return false;
    }

    string file =
        cacheFilePath(
            genre,
            questionCount
        );

    ofstream output(
        file,
        ios::trunc
    );

    if (!output)
    {
        cerr
            << "Could not write cache file: "
            << file
            << endl;

        return false;
    }

    /*
     * File format:
     *
     * CREATED <unix timestamp>
     * LASTUSED <unix timestamp>
     *
     * QUESTION
     * ...
     * OPTION
     * ...
     * ANSWER
     *
     */

    long long now =
        currentUnixTime();

    output
        << "CREATED "
        << now
        << "\n";

    output
        << "LASTUSED "
        << now
        << "\n";

    output
        << "COUNT "
        << questions.size()
        << "\n";

    for (
        const Quiz& quiz :
        questions
    )
    {
        output
            << "QUESTION\n";

        output
            << quiz.question
            << "\n";

        for (
            const string& option :
            quiz.options
        )
        {
            output
                << "OPTION "
                << option
                << "\n";
        }

        output
            << "ANSWER "
            << quiz.answer
            << "\n";

        output
            << "ENDQUESTION\n";
    }

    output.close();

    return true;
}

bool loadQuizFromCache(
    const string& genre,
    int questionCount,
    CacheQuiz& cache
)
{
    lock_guard<mutex> lock(cache_mutex);

    string file =
        cacheFilePath(
            genre,
            questionCount
        );

    if (!fs::exists(file))
    {
        return false;
    }

    ifstream input(file);

    if (!input)
    {
        return false;
    }

    string line;

    Quiz currentQuiz;

    bool readingQuestion = false;

    while (getline(input, line))
    {
        line = trim(line);

        if (line.empty())
        {
            continue;
        }

        if (
            line.rfind(
                "CREATED ",
                0
            ) == 0
        )
        {
            try
            {
                cache.createdAt =
                    stoll(
                        trim(
                            line.substr(8)
                        )
                    );
            }
            catch (...)
            {
                return false;
            }

            continue;
        }

        if (
            line.rfind(
                "LASTUSED ",
                0
            ) == 0
        )
        {
            try
            {
                cache.lastUsed =
                    stoll(
                        trim(
                            line.substr(9)
                        )
                    );
            }
            catch (...)
            {
                return false;
            }

            continue;
        }

        if (line == "QUESTION")
        {
            if (
                readingQuestion &&
                !currentQuiz.question.empty()
            )
            {
                cache.questions.push_back(
                    currentQuiz
                );
            }

            currentQuiz = Quiz{};

            readingQuestion = true;

            continue;
        }

        if (
            line.rfind(
                "OPTION ",
                0
            ) == 0
        )
        {
            if (readingQuestion)
            {
                currentQuiz.options.push_back(
                    line.substr(7)
                );
            }

            continue;
        }

        if (
            line.rfind(
                "ANSWER ",
                0
            ) == 0
        )
        {
            if (readingQuestion)
            {
                currentQuiz.answer =
                    trim(
                        line.substr(7)
                    );
            }

            continue;
        }

        if (line == "ENDQUESTION")
        {
            if (
                readingQuestion &&
                !currentQuiz.question.empty()
            )
            {
                cache.questions.push_back(
                    currentQuiz
                );
            }

            currentQuiz = Quiz{};

            readingQuestion = false;

            continue;
        }

        if (
            readingQuestion &&
            currentQuiz.question.empty()
        )
        {
            currentQuiz.question =
                line;
        }
    }

    input.close();

    // Handle final question if ENDQUESTION
    // was missing.
    if (
        readingQuestion &&
        !currentQuiz.question.empty()
    )
    {
        cache.questions.push_back(
            currentQuiz
        );
    }

    if (
        cache.questions.size() !=
        static_cast<size_t>(
            questionCount
        )
    )
    {
        return false;
    }

    for (
        const Quiz& q :
        cache.questions
    )
    {
        if (
            q.question.empty() ||
            q.options.size() != 4 ||
            q.answer.empty()
        )
        {
            return false;
        }
    }

    return true;
}

bool updateCacheLastUsed(
    const string& genre,
    int questionCount
)
{
    lock_guard<mutex> lock(cache_mutex);

    string file =
        cacheFilePath(
            genre,
            questionCount
        );

    if (!fs::exists(file))
    {
        return false;
    }

    ifstream input(file);

    if (!input)
    {
        return false;
    }

    vector<string> lines;

    string line;

    while (getline(input, line))
    {
        if (
            line.rfind(
                "LASTUSED ",
                0
            ) == 0
        )
        {
            line =
                "LASTUSED " +
                to_string(
                    currentUnixTime()
                );
        }

        lines.push_back(line);
    }

    input.close();

    ofstream output(
        file,
        ios::trunc
    );

    if (!output)
    {
        return false;
    }

    for (
        const string& currentLine :
        lines
    )
    {
        output
            << currentLine
            << "\n";
    }

    output.close();

    return true;
}

// ============================================================
// GET QUIZ USING CACHE
// ============================================================

vector<Quiz> getQuizFromCache(
    const string& rollno,
    const string& genre,
    int questionCount,
    int clientSocket
)
{
    CacheQuiz cachedQuiz;

    bool found =
        loadQuizFromCache(
            genre,
            questionCount,
            cachedQuiz
        );

    if (found)
    {
        long long now =
            currentUnixTime();

        long long age =
            now -
            cachedQuiz.createdAt;

        if (
            age < CACHE_EXPIRY_SECONDS
        )
        {
            cout
                << "CACHE HIT: "
                << genre
                << " / "
                << questionCount
                << " questions\n";

            string message =
                "CACHE HIT: Serving quiz from cache for genre "
                + genre
                + " (" +
                to_string(questionCount)
                + " questions)\n";

            send(
                clientSocket,
                message.c_str(),
                message.size(),
                0
            );

            updateCacheLastUsed(
                genre,
                questionCount
            );

            return cachedQuiz.questions;
        }

        cout
            << "CACHE EXPIRED: "
            << genre
            << " / "
            << questionCount
            << " questions\n";

        string expiredMessage =
            "CACHE EXPIRED: Generating a new quiz for genre "
            + genre
            + "\n";

        send(
            clientSocket,
            expiredMessage.c_str(),
            expiredMessage.size(),
            0
        );

        {
            lock_guard<mutex> lock(
                cache_mutex
            );

            string file =
                cacheFilePath(
                    genre,
                    questionCount
                );

            error_code ec;

            fs::remove(
                file,
                ec
            );
        }
    }

    cout
        << "CACHE MISS: Generating new quiz for "
        << genre
        << " / "
        << questionCount
        << " questions\n";

    string missMessage =
        "CACHE MISS: Generating new quiz for genre "
        + genre
        + "\n";

    send(
        clientSocket,
        missMessage.c_str(),
        missMessage.size(),
        0
    );

    vector<Quiz> newQuiz =
        generateQuiz(
            rollno,
            genre,
            questionCount
        );

    if (
        newQuiz.size() ==
        static_cast<size_t>(
            questionCount
        )
    )
    {
        saveQuizToCache(
            genre,
            questionCount,
            newQuiz
        );

        cout
            << "Quiz saved to file cache.\n";
    }
    else
    {
        cerr
            << "Quiz generation did not produce "
            << questionCount
            << " valid questions. "
            << "Not saving to cache.\n";
    }

    return newQuiz;
}

// ============================================================
// SEND FINAL RESULT
// ============================================================

void sendFinalResult(
    int clientSocket,
    int score,
    const string& genre,
    int questionCount
)
{
    string leaderboard =
        buildLeaderboard(
            genre,
            questionCount
        );

    string finalMessage =
        leaderboard +
        "\n"
        "Quiz over final scores available "
        "your final score is " +
        to_string(score) +
        "\n";

    send(
        clientSocket,
        finalMessage.c_str(),
        finalMessage.size(),
        0
    );
}

// ============================================================
// QUIZ HANDLER
// ============================================================

void handle(
    Student student,
    vector<Quiz> quizQuestions
)
{
    int clientSocket =
        student.socket;

    string clientName =
        student.name;

    string genre =
        student.genre;

    int questionCount =
        student.questionCount;

    // --------------------------------------------------------
    // Validate generated quiz
    // --------------------------------------------------------

    if (
        quizQuestions.size() !=
        static_cast<size_t>(
            questionCount
        )
    )
    {
        string error =
            "ERROR: Could not generate exactly "
            + to_string(questionCount)
            + " questions. Please try again.\n";

        send(
            clientSocket,
            error.c_str(),
            error.size(),
            0
        );

        close(clientSocket);

        return;
    }

    // --------------------------------------------------------
    // Initialize score
    // --------------------------------------------------------

    {
        lock_guard<mutex> lock(
            scores_mutex
        );

        Scores[clientName] = 0;
    }

    cout
        << "Client "
        << clientName
        << " connected on socket "
        << clientSocket
        << endl;

    auto quizStart =
        chrono::steady_clock::now();

    char buffer[4096];

    // ========================================================
    // QUESTIONS
    // ========================================================

    for (
        int i = 0;
        i < static_cast<int>(
                quizQuestions.size()
            );
        i++
    )
    {
        // ----------------------------------------------------
        // Overall quiz timeout
        // ----------------------------------------------------

        auto now =
            chrono::steady_clock::now();

        if (
            chrono::duration_cast<
                chrono::seconds
            >(
                now - quizStart
            ).count()
            >= QUIZ_TOTAL_TIME
        )
        {
            int totalScore;

            {
                lock_guard<mutex> lock(
                    scores_mutex
                );

                totalScore =
                    Scores[clientName];
            }

            saveQuizResult(
                getUserIdByUsername(
                    clientName
                ),
                genre,
                questionCount,
                totalScore
            );

            sendFinalResult(
                clientSocket,
                totalScore,
                genre,
                questionCount
            );

            close(clientSocket);

            return;
        }

        // ----------------------------------------------------
        // Build question
        // ----------------------------------------------------

        string qid =
            "Q" +
            to_string(i + 1);

        string questionMessage =
            "QUES " +
            qid +
            "\n" +
            quizQuestions[i].question +
            "\n";

        for (
            size_t j = 0;
            j < quizQuestions[i].options.size();
            j++
        )
        {
            questionMessage +=
                quizQuestions[i].options[j];

            if (
                j + 1 <
                quizQuestions[i].options.size()
            )
            {
                questionMessage += "\n";
            }
        }

        questionMessage += "\n";

        send(
            clientSocket,
            questionMessage.c_str(),
            questionMessage.size(),
            0
        );

        string correctAnswer =
            quizQuestions[i].answer;

        // ----------------------------------------------------
        // Wait for answer
        // ----------------------------------------------------

        bool answered = false;

        auto lastKeepAlive =
            chrono::steady_clock::now();

        bool keepAliveSent = false;

        while (!answered)
        {

            fd_set readfds;

            FD_ZERO(&readfds);

            FD_SET(
                clientSocket,
                &readfds
            );

            auto currentTime =
                chrono::steady_clock::now();

            int elapsed =
                static_cast<int>(
                    chrono::duration_cast<
                        chrono::seconds
                    >(
                        currentTime -
                        lastKeepAlive
                    ).count()
                );

            int timeout =
                KEEP_ALIVE_INTERVAL -
                elapsed;

            if (timeout < 0)
            {
                timeout = 0;
            }

            timeval tv{};

            tv.tv_sec =
                timeout;

            tv.tv_usec = 0;

            int activity =
                select(
                    clientSocket + 1,
                    &readfds,
                    nullptr,
                    nullptr,
                    &tv
                );

            if (
                activity > 0 &&
                FD_ISSET(
                    clientSocket,
                    &readfds
                )
            )
            {
                memset(
                    buffer,
                    0,
                    sizeof(buffer)
                );

                int bytes =
                    read(
                        clientSocket,
                        buffer,
                        sizeof(buffer) - 1
                    );

                if (bytes <= 0)
                {
                    cout
                        << "Client "
                        << clientName
                        << " disconnected.\n";

                    close(clientSocket);

                    return;
                }

                buffer[bytes] = '\0';

                string response =
                    trim(
                        string(buffer)
                    );

                // ------------------------------------------------
                // HEARTBEAT RESPONSE
                // ------------------------------------------------

                if (
                    response.find(
                        "ALIVE_OK"
                    ) != string::npos
                )
                {
                    lastKeepAlive =
                        chrono::steady_clock::now();

                    keepAliveSent = false;

                    continue;
                }

                // ------------------------------------------------
                // ANSWER
                // ------------------------------------------------

                if (response.empty())
                {
                    continue;
                }

                char clientOption =
                    static_cast<char>(
                        toupper(
                            static_cast<unsigned char>(
                                response[0]
                            )
                        )
                    );

                string result;

                bool validOption =
                    clientOption == 'A' ||
                    clientOption == 'B' ||
                    clientOption == 'C' ||
                    clientOption == 'D';

                char correctOption =
                    static_cast<char>(
                        toupper(
                            static_cast<unsigned char>(
                                correctAnswer[0]
                            )
                        )
                    );

                if (
                    validOption &&
                    clientOption ==
                        correctOption
                )
                {
                    result =
                        "Correct!\n";

                    lock_guard<mutex> lock(
                        scores_mutex
                    );

                    Scores[clientName]++;
                }
                else
                {
                    result =
                        "Wrong!\n";
                }

                send(
                    clientSocket,
                    result.c_str(),
                    result.size(),
                    0
                );

                // ------------------------------------------------
                // Ask whether user wants another question
                // ------------------------------------------------

                string ask =
                    "Do you want Question or do you want to see the Leaderboard\n";

                send(
                    clientSocket,
                    ask.c_str(),
                    ask.size(),
                    0
                );

                // ------------------------------------------------
                // Wait for Question / Leaderboard
                // ------------------------------------------------

                memset(
                    buffer,
                    0,
                    sizeof(buffer)
                );

                int bytes2 =
                    read(
                        clientSocket,
                        buffer,
                        sizeof(buffer) - 1
                    );

                if (bytes2 <= 0)
                {
                    close(clientSocket);

                    return;
                }

                buffer[bytes2] = '\0';

                string choice =
                    trim(
                        string(buffer)
                    );

                if (
                    choice.find(
                        "Leaderboard"
                    ) != string::npos
                )
                {
                    give_leaderboard(
                        clientSocket,
                        genre,
                        questionCount
                    );
                }

                answered = true;

                // ------------------------------------------------
                // Final question
                // ------------------------------------------------

                if (
                    i ==
                    static_cast<int>(
                        quizQuestions.size()
                    ) - 1
                )
                {
                    int totalScore;

                    {
                        lock_guard<mutex> lock(
                            scores_mutex
                        );

                        totalScore =
                            Scores[clientName];
                    }

                    int userId =
                        getUserIdByUsername(
                            clientName
                        );

                    if (userId != -1)
                    {
                        saveQuizResult(
                            userId,
                            genre,
                            questionCount,
                            totalScore
                        );
                    }

                    sendFinalResult(
                        clientSocket,
                        totalScore,
                        genre,
                        questionCount
                    );

                    close(clientSocket);

                    cout
                        << "Client "
                        << clientName
                        << " finished quiz.\n";

                    return;
                }
            }
            else
            {
                // ------------------------------------------------
                // KEEP_ALIVE
                // ------------------------------------------------

                if (!keepAliveSent)
                {
                    string keepAlive =
                        "KEEP_ALIVE\n";

                    send(
                        clientSocket,
                        keepAlive.c_str(),
                        keepAlive.size(),
                        0
                    );

                    keepAliveSent = true;

                    lastKeepAlive =
                        chrono::steady_clock::now();
                }
                else
                {
                    auto current =
                        chrono::steady_clock::now();

                    long long aliveWait =
                        chrono::duration_cast<
                            chrono::seconds
                        >(
                            current -
                            lastKeepAlive
                        ).count();

                    if (
                        aliveWait >=
                        KEEP_ALIVE_TIMEOUT
                    )
                    {
                        cout
                            << "Client "
                            << clientName
                            << " did not respond "
                            << "to KEEP_ALIVE. "
                            << "Terminating quiz.\n";

                        close(clientSocket);

                        return;
                    }
                }
            }
        }
    }

    // --------------------------------------------------------
    // Safety fallback
    // --------------------------------------------------------

    int totalScore;

    {
        lock_guard<mutex> lock(
            scores_mutex
        );

        totalScore =
            Scores[clientName];
    }

    int userId =
        getUserIdByUsername(
            clientName
        );

    if (userId != -1)
    {
        saveQuizResult(
            userId,
            genre,
            questionCount,
            totalScore
        );
    }

    sendFinalResult(
        clientSocket,
        totalScore,
        genre,
        questionCount
    );

    close(clientSocket);

    cout
        << "Client "
        << clientName
        << " finished quiz.\n";
}

// ============================================================
// SERVER CACHE CHOICE
// ============================================================

bool askServerForCacheChoice()
{
    while (true)
    {
        cout
            << "\n========================================\n";

        cout
            << "LLM CACHE CONFIGURATION\n";

        cout
            << "========================================\n";

        cout
            << "Do you want to use LLM cache? (yes/no): ";

        string choice;

        cin >> choice;

        choice =
            toLower(
                trim(choice)
            );

        if (
            choice == "yes" ||
            choice == "y"
        )
        {
            cout
                << "Cache mode: ENABLED\n";

            return true;
        }

        if (
            choice == "no" ||
            choice == "n"
        )
        {
            cout
                << "Cache mode: DISABLED\n";

            return false;
        }

        cout
            << "Please enter yes or no.\n";
    }
}

// ============================================================
// MAIN
// ============================================================

int main()
{
    cout
        << "========================================\n";

    cout
        << "Multi-Client LLM Quiz Server\n";

    cout
        << "========================================\n";

    // --------------------------------------------------------
    // Initialize SQLite
    // --------------------------------------------------------

    if (!initializeDatabase())
    {
        return 1;
    }

    // --------------------------------------------------------
    // CURL global initialization
    // --------------------------------------------------------

    curl_global_init(
        CURL_GLOBAL_DEFAULT
    );

    // --------------------------------------------------------
    // Create cache directory
    // --------------------------------------------------------

    try
    {
        fs::create_directories(
            CACHE_DIRECTORY
        );
    }
    catch (...)
    {
        cerr
            << "Warning: could not create cache directory.\n";
    }

    // --------------------------------------------------------
    // Create server socket
    // --------------------------------------------------------

    int serverSocket =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (serverSocket < 0)
    {
        perror("socket");

        curl_global_cleanup();

        return 1;
    }

    // Allow quick restart after server termination.
    int reuse = 1;

    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in address{};

    address.sin_family =
        AF_INET;

    address.sin_addr.s_addr =
        INADDR_ANY;

    address.sin_port =
        htons(PORT);

    // --------------------------------------------------------
    // Bind
    // --------------------------------------------------------

    if (
        bind(
            serverSocket,
            reinterpret_cast<
                sockaddr*
            >(&address),
            sizeof(address)
        ) < 0
    )
    {
        perror("bind");

        close(serverSocket);

        curl_global_cleanup();

        return 1;
    }

    // --------------------------------------------------------
    // Listen
    // --------------------------------------------------------

    if (
        listen(
            serverSocket,
            MAX_CLIENTS
        ) < 0
    )
    {
        perror("listen");

        close(serverSocket);

        curl_global_cleanup();

        return 1;
    }

    cout
        << "Server listening on port "
        << PORT
        << endl;

    cout
        << "LLM API: "
        << LLM_BASE_URL
        << endl;

    cout
        << "Cache directory: "
        << CACHE_DIRECTORY
        << endl;

    // ========================================================
    // ACCEPT CLIENTS
    // ========================================================

    while (true)
    {
        sockaddr_in clientAddress{};

        socklen_t addressLength =
            sizeof(clientAddress);

        int clientSocket =
            accept(
                serverSocket,
                reinterpret_cast<
                    sockaddr*
                >(&clientAddress),
                &addressLength
            );

        if (clientSocket < 0)
        {
            perror("accept");

            continue;
        }

        char clientIP[
            INET_ADDRSTRLEN
        ];

        inet_ntop(
            AF_INET,
            &clientAddress.sin_addr,
            clientIP,
            INET_ADDRSTRLEN
        );

        int clientPort =
            ntohs(
                clientAddress.sin_port
            );

        cout
            << "\nClient connected from "
            << clientIP
            << ":"
            << clientPort
            << endl;

        // ----------------------------------------------------
        // Receive:
        //
        // gmail|username|genre|questionCount
        // ----------------------------------------------------

        char buffer[4096];

        memset(
            buffer,
            0,
            sizeof(buffer)
        );

        int bytes =
            read(
                clientSocket,
                buffer,
                sizeof(buffer) - 1
            );

        if (bytes <= 0)
        {
            close(clientSocket);

            continue;
        }

        buffer[bytes] = '\0';

        string clientMessage =
            trim(
                string(buffer)
            );

        // ----------------------------------------------------
        // Parse client message
        // ----------------------------------------------------

        vector<string> fields;

        stringstream stream(
            clientMessage
        );

        string field;

        while (
            getline(
                stream,
                field,
                '|'
            )
        )
        {
            fields.push_back(
                trim(field)
            );
        }

        if (fields.size() != 4)
        {
            string error =
                "ERROR: Invalid connection format. "
                "Expected Gmail|Username|Genre|QuestionCount\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        string gmail =
            fields[0];

        string username =
            fields[1];

        string genre =
            fields[2];

        int questionCount = 0;

        try
        {
            questionCount =
                stoi(
                    fields[3]
                );
        }
        catch (...)
        {
            string error =
                "ERROR: Invalid question count.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        // ----------------------------------------------------
        // Validate Gmail
        // ----------------------------------------------------

        if (!validGmail(gmail))
        {
            string error =
                "ERROR: Please enter a valid Gmail address.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        // ----------------------------------------------------
        // Validate username
        // ----------------------------------------------------

        if (username.empty())
        {
            string error =
                "ERROR: Username cannot be empty.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        // ----------------------------------------------------
        // Validate genre
        // ----------------------------------------------------

        if (genre.empty())
        {
            string error =
                "ERROR: Genre cannot be empty.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        // ----------------------------------------------------
        // Validate question count
        // ----------------------------------------------------

        if (!validQuestionCount(questionCount))
        {
            string error =
                "ERROR: Question count must be 5, 10, or 15.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        cout
            << "Gmail: "
            << gmail
            << endl;

        cout
            << "Username: "
            << username
            << endl;

        cout
            << "Genre: "
            << genre
            << endl;

        cout
            << "Question count: "
            << questionCount
            << endl;

        // ----------------------------------------------------
        // Register/login
        // ----------------------------------------------------

        int userId = -1;

        string registrationError;

        if (
            !registerOrLoginUser(
                gmail,
                username,
                userId,
                registrationError
            )
        )
        {
            string error =
                "ERROR: " +
                registrationError +
                "\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        cout
            << "User registration/login successful. "
            << "User ID: "
            << userId
            << endl;

        string registeredMessage =
            "REGISTERED\n";

        send(
            clientSocket,
            registeredMessage.c_str(),
            registeredMessage.size(),
            0
        );

        // ====================================================
        // SERVER OPERATOR CHOOSES CACHE
        // ====================================================

        bool useCache =
            askServerForCacheChoice();

        vector<Quiz> quizQuestions;

        // ----------------------------------------------------
        // Current coursework roll number / reservation ID
        // ----------------------------------------------------

        string rollno =
            "cs23btech11027";

        // ----------------------------------------------------
        // CACHE ENABLED
        // ----------------------------------------------------

        if (useCache)
        {
            quizQuestions =
                getQuizFromCache(
                    rollno,
                    genre,
                    questionCount,
                    clientSocket
                );
        }

        // ----------------------------------------------------
        // CACHE DISABLED
        // ----------------------------------------------------

        else
        {
            cout
                << "CACHE DISABLED: "
                << "Generating quiz directly from LLM.\n";

            string cacheMessage =
                "CACHE DISABLED: Generating new quiz directly from LLM\n";

            send(
                clientSocket,
                cacheMessage.c_str(),
                cacheMessage.size(),
                0
            );

            quizQuestions =
                generateQuiz(
                    rollno,
                    genre,
                    questionCount
                );
        }

        // ----------------------------------------------------
        // Verify question count
        // ----------------------------------------------------

        if (
            quizQuestions.size() !=
            static_cast<size_t>(
                questionCount
            )
        )
        {
            cerr
                << "Quiz generation failed. "
                << "Expected "
                << questionCount
                << " questions but received "
                << quizQuestions.size()
                << ".\n";

            string error =
                "ERROR: Quiz generation failed. "
                "Please try again.\n";

            send(
                clientSocket,
                error.c_str(),
                error.size(),
                0
            );

            close(clientSocket);

            continue;
        }

        cout
            << "Quiz ready with "
            << quizQuestions.size()
            << " questions.\n";

        // ----------------------------------------------------
        // Start client thread
        // ----------------------------------------------------

        Student student{
            gmail,
            username,
            genre,
            questionCount,
            clientSocket
        };

        thread clientThread(
            handle,
            student,
            quizQuestions
        );

        clientThread.detach();
    }

    // This is never normally reached.
    close(serverSocket);

    sqlite3_close(database);

    curl_global_cleanup();

    return 0;
}