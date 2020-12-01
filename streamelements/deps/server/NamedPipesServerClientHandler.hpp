#pragma once

//#include <windows.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include "streamelements/deps/moodycamel/blockingconcurrentqueue.h"

// Single named pipes client handler.
//
// Instantiated and managed by NamedPipesServer class.
//
// Provides facility to send messages to the connected client.
// Provides callback to handle incoming message from client.
//
class NamedPipesServerClientHandler
{
public:
	typedef std::function<void(const char* const, const size_t)> msg_handler_t;

public:
	NamedPipesServerClientHandler(int pipe_in, int pipe_out, msg_handler_t msgHandler);
	virtual ~NamedPipesServerClientHandler();

public:
	bool IsConnected();
	void Disconnect();
	bool WriteMessage(const char* const buffer, size_t length);

private:
	void CallbackThreadProc();
	void ThreadProc();

private:
	int m_pipe[2];
	std::thread m_thread;
	std::thread m_callback_thread;
	msg_handler_t m_msgHandler;
	std::recursive_mutex m_mutex;

	moodycamel::BlockingConcurrentQueue<std::vector<char>> m_writeQueue;
	moodycamel::BlockingConcurrentQueue<std::vector<char>> m_readQueue;
};

