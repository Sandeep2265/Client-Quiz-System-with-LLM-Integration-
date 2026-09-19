"""
WebSocket <-> TCP bridge for the coursework quiz server.

Your C++ server (server.cpp / server1.cpp) speaks a raw TCP socket protocol,
which browsers cannot connect to directly. This bridge sits in between:

    Browser  <--WebSocket-->  bridge.py  <--TCP socket-->  server.cpp

It does nothing except forward bytes in both directions - no quiz logic,
no LLM calls, no changes to your existing C++ server or its protocol.

Usage:
    1. Compile and run your existing C++ server as normal:
           g++ server.cpp -o server -lcurl -lpthread
           ./server
       (it will print "Server listening on port 11027")

    2. In another terminal, run this bridge:
           pip install websockets
           python3 bridge.py

    3. Open index.html in a browser (see that file for details).
"""

import asyncio
import logging

import websockets

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Where your C++ server (server.cpp / server1.cpp) is listening.
CPP_SERVER_HOST = "127.0.0.1"
CPP_SERVER_PORT = 11027

# Where this bridge listens for browser WebSocket connections.
BRIDGE_HOST = "0.0.0.0"
BRIDGE_PORT = 8765


async def handle_client(websocket):
    """
    One browser tab = one WebSocket connection = one TCP connection to the
    C++ server. The first message from the browser must be "name|genre",
    exactly like the C++ client sends on connect.
    """
    writer = None
    peer = websocket.remote_address
    logger.info(f"Browser connected: {peer}")

    try:
        # Wait for the browser to send "name|genre" before opening the
        # TCP connection, so we forward it as the very first bytes -
        # exactly what server.cpp's main() expects to read first.
        first_message = await websocket.recv()

        reader, writer = await asyncio.open_connection(CPP_SERVER_HOST, CPP_SERVER_PORT)
        writer.write(first_message.encode("utf-8"))
        await writer.drain()
        logger.info(f"Connected to C++ server, sent join message: {first_message!r}")

        async def tcp_to_ws():
            """Forward everything the C++ server sends to the browser."""
            try:
                while True:
                    data = await reader.read(4096)
                    if not data:
                        logger.info("C++ server closed the connection")
                        break
                    await websocket.send(data.decode("utf-8", errors="ignore"))
            except (websockets.exceptions.ConnectionClosed, asyncio.CancelledError):
                pass

        async def ws_to_tcp():
            """Forward everything the browser sends (answers, ALIVE_OK, etc.) to the C++ server."""
            try:
                async for message in websocket:
                    writer.write(message.encode("utf-8"))
                    await writer.drain()
            except (websockets.exceptions.ConnectionClosed, asyncio.CancelledError):
                pass

        # Run both directions concurrently; stop when either side ends.
        done, pending = await asyncio.wait(
            [asyncio.create_task(tcp_to_ws()), asyncio.create_task(ws_to_tcp())],
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()

    except websockets.exceptions.ConnectionClosed:
        logger.info(f"Browser disconnected: {peer}")
    except ConnectionRefusedError:
        logger.error(
            f"Could not connect to C++ server at {CPP_SERVER_HOST}:{CPP_SERVER_PORT}. "
            "Is server.cpp running?"
        )
        try:
            await websocket.send("ERROR: Quiz server is not running. Ask your admin to start server.cpp.\n")
        except Exception:
            pass
    except Exception as e:
        logger.error(f"Bridge error: {e}")
    finally:
        if writer:
            writer.close()
        logger.info(f"Session ended: {peer}")


async def main():
    print("=" * 60)
    print("Quiz WebSocket <-> TCP Bridge")
    print("=" * 60)
    print(f"Listening for browsers on ws://localhost:{BRIDGE_PORT}")
    print(f"Forwarding to C++ quiz server at {CPP_SERVER_HOST}:{CPP_SERVER_PORT}")
    print("Make sure server.cpp is already running before connecting.")
    print("=" * 60)

    async with websockets.serve(handle_client, BRIDGE_HOST, BRIDGE_PORT):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
