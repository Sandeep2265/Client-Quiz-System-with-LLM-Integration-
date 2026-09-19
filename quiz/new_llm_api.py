"""
Replacement for the professor's private quiz-question API
(http://192.168.50.142:25000).

server.cpp's generateQuiz() function calls two endpoints:
  POST /reserve        {"rollno": "..."}
  POST /generate_quiz  {"rollno": "...", "messages": [...], "temperature": ..., "max_tokens": ...}

and expects /generate_quiz to return an OpenAI-style chat completion:
  {"choices": [{"message": {"content": "1. Question...\\nA) ...\\nAnswer: X\\n..."}}]}

This service reproduces exactly that contract, but generates the actual
questions with Groq instead of the professor's backend. Because the shape
matches, server.cpp's existing parsing code in generateQuiz() does not need
to change at all - only the base URL it calls.

Setup:
    pip install fastapi uvicorn httpx python-dotenv
    echo "GROQ_API_KEY=gsk_your_key_here" > .env
    python3 new_llm_api.py
    (listens on http://0.0.0.0:9000)
"""

import os
import logging

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import List, Dict, Any

load_dotenv()

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
GROQ_MODEL = os.getenv("GROQ_MODEL", "openai/gpt-oss-20b")
GROQ_URL = "https://api.groq.com/openai/v1/chat/completions"

app = FastAPI(title="Quiz Question API (Groq-backed replacement)")


class ReserveRequest(BaseModel):
    rollno: str


class Message(BaseModel):
    role: str
    content: str


class GenerateQuizRequest(BaseModel):
    rollno: str
    messages: List[Message]
    temperature: float = 0.0
    max_tokens: int = 1200


@app.post("/reserve")
async def reserve(request: ReserveRequest):
    """
    The original API used this to rate-limit students sharing one quota.
    This replacement has no such limit, so it always succeeds -
    server.cpp's reserve() function will treat this as a normal success.
    """
    logger.info(f"Reserve requested for rollno={request.rollno}")
    return {"status": "success"}


@app.post("/generate_quiz")
async def generate_quiz(request: GenerateQuizRequest):
    """
    Forwards the exact messages server.cpp already built (its system prompt
    already asks for the "1. Question\\nA) ...\\nAnswer: X" format) straight
    to Groq, and returns the response in the same {"choices": [...]} shape
    the original API used - so server.cpp's parser needs no changes.
    """
    if not GROQ_API_KEY:
        raise HTTPException(status_code=500, detail="GROQ_API_KEY is not set. Add it to .env.")

    payload = {
        "model": GROQ_MODEL,
        "messages": [m.dict() for m in request.messages],
        "temperature": request.temperature,
        "max_tokens": min(request.max_tokens, 4096),
    }
    headers = {
        "Authorization": f"Bearer {GROQ_API_KEY}",
        "Content-Type": "application/json",
    }

    try:
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(GROQ_URL, headers=headers, json=payload)
    except httpx.RequestError as e:
        logger.error(f"Groq request failed: {e}")
        raise HTTPException(status_code=502, detail="Could not reach Groq API.")

    if resp.status_code != 200:
        logger.error(f"Groq API error {resp.status_code}: {resp.text}")
        raise HTTPException(status_code=502, detail=f"Groq API error {resp.status_code}")

    groq_data = resp.json()

    # Re-shape into exactly what server.cpp expects: choices[0].message.content
    try:
        content = groq_data["choices"][0]["message"]["content"]
    except (KeyError, IndexError):
        logger.error(f"Unexpected Groq response: {groq_data}")
        raise HTTPException(status_code=502, detail="Unexpected response from Groq.")

    return {"choices": [{"message": {"role": "assistant", "content": content}}]}


@app.get("/health")
async def health():
    return {"status": "ok", "groq_configured": bool(GROQ_API_KEY)}


if __name__ == "__main__":
    import uvicorn

    print("=" * 60)
    print("Quiz Question API (Groq-backed replacement for professor's API)")
    print("=" * 60)
    print("Listening on http://0.0.0.0:9000")
    print(f"Groq key set: {bool(GROQ_API_KEY)}")
    print("Point server.cpp's httpPostRequest calls at:")
    print("  http://127.0.0.1:9000/reserve")
    print("  http://127.0.0.1:9000/generate_quiz")
    print("=" * 60)

    uvicorn.run(app, host="0.0.0.0", port=9000, log_level="info")
