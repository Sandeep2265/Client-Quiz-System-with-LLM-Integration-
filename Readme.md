# Multi client Quiz Application

A full-stack quiz application using **React, C++, Python, SQLite, and Groq**.

## Architecture

```text
React + Vite
     |
 WebSocket :8765
     |
  bridge.py
     |
 TCP :11027
     |
 server.cpp
     |
 HTTP :9000
     |
new_llm_api.py
     |
   Groq API
```

## Features

* AI-generated quiz questions using Groq
* 5, 10, or 15 questions
* Custom quiz genres
* Gmail and username registration
* Persistent SQLite leaderboard
* Separate leaderboards by genre and question count
* Optional LLM cache
* WebSocket/TCP communication
* Keep-alive mechanism

## Project Structure

```text
quiz/
├── server.cpp
├── bridge.py
├── new_llm_api.py
├── requirements_web_client.txt
├── README.md
├── .env
└── frontend/
    ├── package.json
    └── src/
        ├── App.jsx
        └── App.css
```

`client.cpp` and `client1.cpp` are not required for the web application.

## Requirements

* Python 3
* Node.js + npm
* g++
* libcurl
* SQLite
* Groq API key

On Fedora:

```bash
sudo dnf install python3 python3-pip nodejs npm gcc-c++ libcurl-devel sqlite-devel
```

## Setup

Clone the repository:

```bash
git clone <YOUR_REPOSITORY_URL>
cd quiz
```

Create a Python environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements_web_client.txt
```

Install frontend dependencies:

```bash
cd frontend
npm install
cd ..
```

Create `.env` in the project root:

```env
GROQ_API_KEY=YOUR_GROQ_API_KEY
GROQ_MODEL=openai/gpt-oss-20b
```

Compile the C++ server:

```bash
g++ server.cpp -o server -lcurl -lsqlite3 -lpthread
```

## Run

The application requires four terminals.

### Terminal 1 — AI API

```bash
source .venv/bin/activate
python3 new_llm_api.py
```

Runs on port `9000`.

### Terminal 2 — C++ Server

```bash
./server
```

The server asks:

```text
Use LLM cache? (y/n):
```

Choose `y` or `n`.

Runs on port `11027`.

### Terminal 3 — WebSocket Bridge

```bash
source .venv/bin/activate
python3 bridge.py
```

Runs on port `8765`.

### Terminal 4 — React

```bash
cd frontend
npm run dev -- --host 0.0.0.0
```

Open:

```text
http://localhost:5173
```
