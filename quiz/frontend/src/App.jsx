import { useEffect, useRef, useState } from "react";
import "./App.css";

const WS_URL = `${
  window.location.protocol === "https:" ? "wss:" : "ws:"
}//${window.location.hostname}:8765`;

function App() {
  const socketRef = useRef(null);

  const [screen, setScreen] = useState("register");

  const [gmail, setGmail] = useState("");
  const [username, setUsername] = useState("");
  const [genre, setGenre] = useState("");
  const [questionCount, setQuestionCount] = useState(5);

  const [questionNumber, setQuestionNumber] = useState(0);
  const [question, setQuestion] = useState("");
  const [options, setOptions] = useState([]);
  const [selectedAnswer, setSelectedAnswer] = useState("");
  const [feedback, setFeedback] = useState("");

  const [score, setScore] = useState(0);
  const [leaderboard, setLeaderboard] = useState("");

  const [error, setError] = useState("");
  const [status, setStatus] = useState("");

  useEffect(() => {
    return () => {
      if (socketRef.current) {
        socketRef.current.close();
      }
    };
  }, []);

  // ==========================================================
  // CONNECT TO SERVER
  // ==========================================================

  function connectAndStart() {
    setError("");
    setStatus("Connecting...");

    const cleanGmail = gmail.trim();
    const cleanUsername = username.trim();
    const cleanGenre = genre.trim();

    if (!cleanGmail) {
      setError("Please enter your Gmail ID.");
      setStatus("");
      return;
    }

    if (!cleanUsername) {
      setError("Please enter a username.");
      setStatus("");
      return;
    }

    if (!cleanGenre) {
      setError("Please enter a genre.");
      setStatus("");
      return;
    }

    if (
      !/^[A-Za-z0-9._%+-]+@gmail\.com$/i.test(
        cleanGmail
      )
    ) {
      setError("Please enter a valid Gmail address.");
      setStatus("");
      return;
    }

    if (![5, 10, 15].includes(Number(questionCount))) {
      setError("Please select 5, 10, or 15 questions.");
      setStatus("");
      return;
    }

    const socket = new WebSocket(WS_URL);

    socketRef.current = socket;

    // ----------------------------------------------------------
    // WebSocket opened
    // ----------------------------------------------------------

    socket.onopen = () => {
      setStatus("Connected");

      /*
       * C++ server expects:
       *
       * gmail|username|genre|question_count
       *
       * Example:
       *
       * sandeep@gmail.com|Sandeep|Science|10
       *
       * Cache choice is NOT sent from React.
       * The C++ server asks for cache usage
       * in its terminal.
       */

      const joinMessage =
        `${cleanGmail}|${cleanUsername}|${cleanGenre}|${questionCount}`;

      console.log("Sending:", joinMessage);

      socket.send(joinMessage);

      setScreen("loading");
    };

    // ----------------------------------------------------------
    // Server message
    // ----------------------------------------------------------

    socket.onmessage = (event) => {
      handleServerMessage(event.data);
    };

    // ----------------------------------------------------------
    // WebSocket error
    // ----------------------------------------------------------

    socket.onerror = () => {
      setError(
        "Could not connect to the quiz server. " +
          "Make sure server.cpp and bridge.py are running."
      );

      setStatus("");
      setScreen("register");
    };

    // ----------------------------------------------------------
    // WebSocket closed
    // ----------------------------------------------------------

    socket.onclose = () => {
      setStatus("");
    };
  }

  // ==========================================================
  // HANDLE SERVER MESSAGE
  // ==========================================================

  function handleServerMessage(rawMessage) {
    const message = String(rawMessage).trim();

    console.log("Server:", message);

    if (!message) {
      return;
    }

    // --------------------------------------------------------
    // HEARTBEAT
    // --------------------------------------------------------

    if (message === "KEEP_ALIVE") {
      if (
        socketRef.current?.readyState ===
        WebSocket.OPEN
      ) {
        socketRef.current.send("ALIVE_OK");
        console.log("Sent: ALIVE_OK");
      }

      return;
    }

    // --------------------------------------------------------
    // ERROR
    // --------------------------------------------------------

    if (message.startsWith("ERROR:")) {
      setError(
        message.substring(6).trim()
      );

      setScreen("register");
      setStatus("");

      if (socketRef.current) {
        socketRef.current.close();
      }

      return;
    }

    // --------------------------------------------------------
    // REGISTERED
    // --------------------------------------------------------

    if (message === "REGISTERED") {
      setStatus(
        "Registration successful. Preparing quiz..."
      );

      return;
    }

    // --------------------------------------------------------
    // CACHE STATUS
    //
    // Cache is controlled by server.cpp.
    // These messages are only displayed as status.
    // --------------------------------------------------------

    if (
      message.startsWith("CACHE HIT") ||
      message.startsWith("CACHE MISS") ||
      message.startsWith("CACHE EXPIRED") ||
      message.startsWith("CACHE DISABLED")
    ) {
      console.log("Cache:", message);
      setStatus(message);
      return;
    }

    // --------------------------------------------------------
    // FINAL SCORE
    // --------------------------------------------------------

    const finalScoreMatch = message.match(
      /Quiz over final scores available\s+your final score is\s+(\d+)/i
    );

    if (finalScoreMatch) {
      const finalScore =
        Number(finalScoreMatch[1]);

      setScore(finalScore);
    }

    // --------------------------------------------------------
    // LEADERBOARD
    // --------------------------------------------------------

    const leaderboardIndex =
      message.indexOf("Leaderboard:");

    if (leaderboardIndex !== -1) {
      let leaderboardText =
        message.substring(
          leaderboardIndex
        );

      /*
       * Remove final-score text from
       * the leaderboard display.
       */

      const finalIndex =
        leaderboardText.search(
          /Quiz over final scores available/i
        );

      if (finalIndex !== -1) {
        leaderboardText =
          leaderboardText
            .substring(0, finalIndex)
            .trim();
      }

      setLeaderboard(
        leaderboardText
      );
    }

    // --------------------------------------------------------
    // FINAL RESULT SCREEN
    // --------------------------------------------------------

    if (finalScoreMatch) {
      setScreen("result");
      setFeedback("");
      setSelectedAnswer("");
      setStatus("");

      return;
    }

    // --------------------------------------------------------
    // QUESTION
    // --------------------------------------------------------

    if (/^QUES\s+/i.test(message)) {
      parseQuestion(message);
      return;
    }

    // --------------------------------------------------------
    // CORRECT
    // --------------------------------------------------------

    if (/^Correct!/i.test(message)) {
      setFeedback("Correct!");
      return;
    }

    // --------------------------------------------------------
    // WRONG
    // --------------------------------------------------------

    if (/^Wrong!/i.test(message)) {
      setFeedback("Wrong!");
      return;
    }

    // --------------------------------------------------------
    // SERVER ASKING WHAT TO DO NEXT
    // --------------------------------------------------------

    if (
      message
        .toLowerCase()
        .includes("do you want question") ||
      message
        .toLowerCase()
        .includes(
          "do you want to see the leaderboard"
        )
    ) {
      /*
       * User clicks "Next Question"
       * to send "Question".
       */

      return;
    }
  }

  // ==========================================================
  // PARSE QUESTION
  // ==========================================================

  function parseQuestion(message) {
    const lines = message
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean);

    if (lines.length < 5) {
      console.warn(
        "Invalid question message:",
        message
      );

      return;
    }

    const header = lines[0];

    const numberMatch =
      header.match(
        /^QUES\s+Q?(\d+)/i
      );

    if (numberMatch) {
      setQuestionNumber(
        Number(numberMatch[1])
      );
    }

    const questionText =
      lines[1];

    const parsedOptions = [];

    for (
      const line of lines.slice(2)
    ) {
      const match =
        line.match(
          /^([A-D])[\)\.:]\s*(.+)$/i
        );

      if (match) {
        parsedOptions.push({
          letter:
            match[1].toUpperCase(),

          text:
            match[2].trim(),
        });
      }
    }

    if (parsedOptions.length !== 4) {
      console.warn(
        "Expected 4 options but received:",
        parsedOptions
      );
    }

    setQuestion(
      questionText
    );

    setOptions(
      parsedOptions
    );

    setSelectedAnswer("");
    setFeedback("");
    setError("");
    setStatus("");

    setScreen("quiz");
  }

  // ==========================================================
  // SUBMIT ANSWER
  // ==========================================================

  function submitAnswer(letter) {
    if (!socketRef.current) {
      return;
    }

    if (selectedAnswer) {
      return;
    }

    setSelectedAnswer(letter);

    if (
      socketRef.current.readyState ===
      WebSocket.OPEN
    ) {
      socketRef.current.send(letter);
    }
  }

  // ==========================================================
  // NEXT QUESTION
  // ==========================================================

  function nextQuestion() {
    if (!socketRef.current) {
      return;
    }

    if (
      socketRef.current.readyState ===
      WebSocket.OPEN
    ) {
      socketRef.current.send(
        "Question"
      );
    }

    setFeedback("");
    setSelectedAnswer("");
    setStatus(
      "Loading next question..."
    );
  }

  // ==========================================================
  // RESET
  // ==========================================================

  function resetQuiz() {
    if (socketRef.current) {
      socketRef.current.close();
    }

    setScreen("register");

    setQuestionNumber(0);
    setQuestion("");
    setOptions([]);
    setSelectedAnswer("");
    setFeedback("");

    setScore(0);
    setLeaderboard("");

    setError("");
    setStatus("");
  }

  // ==========================================================
  // REGISTER SCREEN
  // ==========================================================

  function renderRegister() {
    return (
      <div className="card">

        <div className="logo">
          QUIZ
        </div>

        <h1>
          Quiz Challenge
        </h1>

        <p className="subtitle">
          Enter your details to start
          the quiz.
        </p>

        {/* Gmail */}

        <div className="form-group">
          <label>
            Gmail ID
          </label>

          <input
            type="email"
            placeholder="example@gmail.com"
            value={gmail}
            onChange={(e) =>
              setGmail(e.target.value)
            }
          />
        </div>

        {/* Username */}

        <div className="form-group">
          <label>
            Username
          </label>

          <input
            type="text"
            placeholder="Choose a username"
            value={username}
            onChange={(e) =>
              setUsername(e.target.value)
            }
          />
        </div>

        {/* Genre */}

        <div className="form-group">
          <label>
            Genre
          </label>

          <input
            type="text"
            placeholder="e.g. Science, Python, C++, History"
            value={genre}
            onChange={(e) =>
              setGenre(e.target.value)
            }
          />
        </div>

        {/* Question count */}

        <div className="form-group">
          <label>
            Number of Questions
          </label>

          <div className="question-count">

            {[5, 10, 15].map(
              (count) => (
                <label
                  key={count}
                  className={
                    Number(
                      questionCount
                    ) === count
                      ? "count-option selected"
                      : "count-option"
                  }
                >
                  <input
                    type="radio"
                    name="questionCount"
                    value={count}
                    checked={
                      Number(
                        questionCount
                      ) === count
                    }
                    onChange={() =>
                      setQuestionCount(
                        count
                      )
                    }
                  />

                  <span>
                    {count}
                  </span>
                </label>
              )
            )}

          </div>
        </div>

        {/* Error */}

        {error && (
          <div className="error">
            {error}
          </div>
        )}

        {/* Start */}

        <button
          className="primary-button"
          onClick={connectAndStart}
        >
          Start Quiz
        </button>

        {status && (
          <div className="status">
            {status}
          </div>
        )}

      </div>
    );
  }

  // ==========================================================
  // LOADING SCREEN
  // ==========================================================

  function renderLoading() {
    return (
      <div className="card loading-card">

        <div className="spinner"></div>

        <h2>
          Preparing your quiz...
        </h2>

        <p>
          Generating{" "}
          <strong>
            {questionCount}
          </strong>{" "}
          questions for{" "}
          <strong>
            {genre}
          </strong>
          .
        </p>

        {status && (
          <div className="status">
            {status}
          </div>
        )}

      </div>
    );
  }

  // ==========================================================
  // QUIZ SCREEN
  // ==========================================================

  function renderQuiz() {
    const progress =
      Math.min(
        100,
        (questionNumber /
          Number(questionCount)) *
          100
      );

    return (
      <div className="quiz-container">

        <div className="quiz-header">

          <div>
            <span className="small-label">
              Genre
            </span>

            <strong>
              {genre}
            </strong>
          </div>

          <div className="progress">
            Question{" "}
            {questionNumber} /{" "}
            {questionCount}
          </div>

          <div>
            <span className="small-label">
              Score
            </span>

            <strong>
              {score}
            </strong>
          </div>

        </div>

        <div className="progress-bar">

          <div
            className="progress-fill"
            style={{
              width: `${progress}%`,
            }}
          ></div>

        </div>

        <div className="question-card">

          <div className="question-number">
            Question{" "}
            {questionNumber}
          </div>

          <h2>
            {question}
          </h2>

          <div className="options">

            {options.map(
              (option) => {

                const isSelected =
                  selectedAnswer ===
                  option.letter;

                return (
                  <button
                    key={
                      option.letter
                    }
                    className={
                      isSelected
                        ? "answer-button selected-answer"
                        : "answer-button"
                    }
                    disabled={
                      Boolean(
                        selectedAnswer
                      )
                    }
                    onClick={() =>
                      submitAnswer(
                        option.letter
                      )
                    }
                  >

                    <span className="answer-letter">
                      {option.letter}
                    </span>

                    <span>
                      {option.text}
                    </span>

                  </button>
                );
              }
            )}

          </div>

          {/* Feedback */}

          {feedback && (
            <div
              className={
                feedback ===
                "Correct!"
                  ? "feedback correct"
                  : "feedback wrong"
              }
            >
              {feedback}
            </div>
          )}

          {/* Next */}

          {selectedAnswer && (
            <button
              className="next-button"
              onClick={
                nextQuestion
              }
            >
              Next Question
            </button>
          )}

        </div>

      </div>
    );
  }

  // ==========================================================
  // RESULT SCREEN
  // ==========================================================

  function renderResult() {
    return (
      <div className="result-container">

        <div className="result-card">

          <div className="result-icon">
            ✓
          </div>

          <h1>
            Quiz Completed
          </h1>

          <p className="result-user">
            Well done,{" "}
            <strong>
              {username}
            </strong>
            .
          </p>

          <div className="score-box">

            <span>
              Your Score
            </span>

            <strong>
              {score} /{" "}
              {questionCount}
            </strong>

          </div>

          {leaderboard && (
            <div className="leaderboard">

              <h2>
                Leaderboard
              </h2>

              <pre>
                {leaderboard}
              </pre>

            </div>
          )}

          <button
            className="primary-button"
            onClick={resetQuiz}
          >
            Take Another Quiz
          </button>

        </div>

      </div>
    );
  }

  // ==========================================================
  // APP
  // ==========================================================

  return (
    <div className="app">

      {screen === "register" &&
        renderRegister()}

      {screen === "loading" &&
        renderLoading()}

      {screen === "quiz" &&
        renderQuiz()}

      {screen === "result" &&
        renderResult()}

    </div>
  );
}

export default App;