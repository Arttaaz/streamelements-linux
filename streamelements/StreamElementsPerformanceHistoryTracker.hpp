#pragma once
#ifdef _WIN32
#include <windows.h>
#endif
#include <util/threading.h>
#include <thread>
#include <mutex>
#include <vector>

class StreamElementsPerformanceHistoryTracker
{
public:
	typedef long double seconds_t;

	struct CpuTime
	{
		seconds_t totalSeconds;
		seconds_t idleSeconds;
		seconds_t busySeconds;
	};
#ifdef _WIN32
	typedef CpuTime cpu_usage_t;
	typedef MEMORYSTATUSEX memory_usage_t;
#endif

public:
	StreamElementsPerformanceHistoryTracker();
	~StreamElementsPerformanceHistoryTracker();
#ifdef _WIN32
	std::vector<memory_usage_t> getMemoryUsageSnapshot();
	std::vector<cpu_usage_t> getCpuUsageSnapshot();
#endif
private:
	std::recursive_mutex m_mutex;

	os_event_t* m_quit_event;
	os_event_t* m_done_event;
#ifdef _WIN32
	std::vector<cpu_usage_t> m_cpu_usage;
	std::vector<memory_usage_t> m_memory_usage;
#endif
};
