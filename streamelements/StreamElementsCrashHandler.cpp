/*
 * This is the global OBS.Live crash handler.
 *
 * It operates by setting up a global exception filter (saving
 * the exception filter previously set by obs.dll).
 *
 * Once an exception occurs:
 *
 * 1. Our exception filter gets called
 * 2. Our exception filter calls the obs.dll exception filter
 *    which composes a crash report
 * 3. Our obs crash reporting callback is called, and saves the
 *    text crash report mimicking obs.dll crash handler behavior.
 *    The text report content is also saved in a global variable
 *    for later retrieval by our BugSplat crash callback.
 *    This is needed since obs.dll does not provide a mechanism
 *    to chain obs.dll crash handlers.
 *    After saving the text crash report, it sends a crash event
 *    to the analytics backend (HEAP).
 * 4. BugSplat crash handler is called
 * 5. Our BugSplat crash callback is called. The callback
 *    sets User Description to the value of the text crash report
 *    generated by obs.dll, and creates a ZIP file containing
 *    user's OBS configuration to send along with the report to
 *    BugSplat servers.
 * 6. Once all this is done, a message box is presented to the user
 *    telling them OBS crashed, and asking whether they'd like the
 *    crash report to be copied to the clipboard.
 * 7. Next the program is terminated
 */

#include "StreamElementsCrashHandler.hpp"
#include "StreamElementsGlobalStateManager.hpp"
#include "deps/StackWalker/StackWalker.h"
#include <util/base.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>
#include <time.h>
#include <cstdio>
#include <string>
#include <ios>
#include <fstream>
#include <codecvt>

#ifdef _WIN32
#include <windows.h>
#include <winuser.h>
#include "bugsplat.h"
#include <io.h>
#endif

#include "deps/zip/zip.h"
#include <iostream>
#include <filesystem>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ================================================================= */

static inline bool HasRandomMatch()
{
	return (os_gettime_ns() / 1000L) % 10 == 0; // 1:10 chance
}

/* ================================================================= */
#ifdef _WIN32
static class MyStackWalker : public StackWalker {
public:
	MyStackWalker(int options) : StackWalker(options)
	{
		output.reserve(1024 * 16);
	}

protected:
	virtual void OnCallstackEntry(CallstackEntryType eType,
				      CallstackEntry &entry) override
	{
		StackWalker::OnCallstackEntry(eType, entry);

		if (!entry.offset)
			return;

#define LOCAL_SPACE "\t"
		output += entry.loadedImageName;
		output += LOCAL_SPACE;
		output += entry.undFullName;
		output += LOCAL_SPACE;
		output += entry.lineFileName;
		output += " (";
		char numbuf[32];
		output += ltoa(entry.lineNumber, numbuf, 10);
		output += ")";
		output += "\n";
#undef LOCAL_SPACE

		if (!hasMatchModuleOfInterest) {
			for (auto filter : modulesOfInterest) {
				if (stricmp(filter.c_str(), entry.moduleName)) {
					hasMatchModuleOfInterest = true;

					break;
				}
			}
		}
	}

public:
	bool hasMatchModuleOfInterest = HasRandomMatch();
	std::vector<std::string> modulesOfInterest;
	std::string output;
};

/* ================================================================= */

static MyStackWalker *s_stackWalker = nullptr;
static MiniDmpSender *s_mdSender = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_prevExceptionFilter = nullptr;
static DWORD s_insideExceptionFilter = 0L;
#endif
static std::string s_crashDumpFromObs;
static std::string s_crashDumpFromStackWalker;

/* ================================================================= */

static void null_crash_handler(const char *format, va_list args, void *param)
{
	exit(-1);

	UNUSED_PARAMETER(format);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(param);
}

static std::string GenerateTimeDateFilename(const char *extension,
					    bool noSpace = false)
{
	time_t now = time(0);
	char file[256] = {};
	struct tm *cur_time;

	cur_time = localtime(&now);
	snprintf(file, sizeof(file), "%d-%02d-%02d%c%02d-%02d-%02d.%s",
		 cur_time->tm_year + 1900, cur_time->tm_mon + 1,
		 cur_time->tm_mday, noSpace ? '_' : ' ', cur_time->tm_hour,
		 cur_time->tm_min, cur_time->tm_sec, extension);

	return std::string(file);
}

static void delete_oldest_file(bool has_prefix, const char *location)
{
	UNUSED_PARAMETER(has_prefix);

	char *basePathPtr = os_get_config_path_ptr(location);
	std::string logDir(basePathPtr);
	bfree(basePathPtr);

	std::string oldestLog;
	time_t oldest_ts = (time_t)-1;
	struct os_dirent *entry;

	unsigned int maxLogs = (unsigned int)config_get_uint(
		obs_frontend_get_global_config(), "General", "MaxLogs");

	os_dir_t *dir = os_opendir(logDir.c_str());
	if (dir) {
		unsigned int count = 0;

		while ((entry = os_readdir(dir)) != NULL) {
			if (entry->directory || *entry->d_name == '.')
				continue;

			std::string filePath =
				logDir + "/" + std::string(entry->d_name);
			struct stat st;
			if (0 == os_stat(filePath.c_str(), &st)) {
				time_t ts = st.st_ctime;

				if (ts) {
					if (ts < oldest_ts) {
						oldestLog = filePath;
						oldest_ts = ts;
					}

					count++;
				}
			}
		}

		os_closedir(dir);

		if (count > maxLogs) {
			os_unlink(oldestLog.c_str());
		}
	}
}

#define MAX_CRASH_REPORT_SIZE (300 * 1024)

#define CRASH_MESSAGE                                                      \
	"Woops, OBS has crashed!\n\nWould you like to copy the crash log " \
	"to the clipboard?  (Crash logs will still be saved to the "       \
	"%appdata%\\obs-studio\\crashes directory)"

static void write_file_content(std::string &path, const char *content)
{
	std::fstream file;

#ifdef _WIN32
	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	std::wstring wpath = myconv.from_bytes(path);

	file.open(wpath, std::ios_base::in | std::ios_base::out |
				 std::ios_base::trunc | std::ios_base::binary);
#else
	file.open(path, std::ios_base::in | std::ios_base::out |
				std::ios_base::trunc | std::ios_base::binary);
#endif
	file << content;

	file.close();
}

///
// StreamElements Crash Handler
//
// Don't use any asynchronous calls here
// Don't use stdio FILE* here
//
// Repeats crash handler functionality found in obs-app.cpp
//
// This is because there is no way to chain two crash handlers
// together at the moment of this writing.
//
// Note: The message box is moved outside this function to the
//       top level exception filter.
//
//       It will still be presented if the handler determines
//       that it is not running within our top level exception
//       filter context.
//
static void main_crash_handler(const char *format, va_list args, void *param)
{
#ifdef _WIN32
	// Allocate space for crash report content
	char *text = new char[MAX_CRASH_REPORT_SIZE];

	// Build crash report
	vsnprintf(text, MAX_CRASH_REPORT_SIZE, format, args);
	text[MAX_CRASH_REPORT_SIZE - 1] = 0;

	s_crashDumpFromObs = text;

	s_crashDumpFromStackWalker +=
		"\n======================================================================\n";
	"\n======================================================================\n";
	s_crashDumpFromStackWalker += "Additional stack info:\n";
	s_crashDumpFromStackWalker +=
		"======================================================================\n\n";

	s_crashDumpFromStackWalker += s_stackWalker->output;

	s_crashDumpFromStackWalker +=
		"\n======================================================================\n";
	s_crashDumpFromStackWalker += "StreamElements Plug-in info:\n";
	s_crashDumpFromStackWalker +=
		"======================================================================\n\n";

	s_crashDumpFromStackWalker += "StreamElements Plug-in Version: " +
				      GetStreamElementsPluginVersionString() +
				      "\n";

	s_crashDumpFromStackWalker +=
		"CEF Version: " + GetCefVersionString() + "\n";
	s_crashDumpFromStackWalker +=
		"CEF API Hash: " + GetCefPlatformApiHash() + "\n";
	s_crashDumpFromStackWalker +=
		"Machine Unique ID: " + GetComputerSystemUniqueId() + "\n";

#ifdef _WIN32
#ifdef _WIN64
	s_crashDumpFromStackWalker += "Platform: Windows (64bit)\n";
#else
	s_crashDumpFromStackWalker += "Platform: Windows (32bit)\n";
#endif
#elif APPLE
	s_crashDumpFromStackWalker += "Platform: MacOS\n";
#elif LINUX
	s_crashDumpFromStackWalker += "Platform: Linux\n";
#else
	s_crashDumpFromStackWalker += "Platform: Other\n";
#endif

	s_crashDumpFromObs += s_crashDumpFromStackWalker;

	// Delete oldest crash report
	delete_oldest_file(true, "obs-studio/crashes");

	// Build output file path
	std::string name = "obs-studio/crashes/Crash ";
	name += GenerateTimeDateFilename("txt");

	char *basePathPtr = os_get_config_path_ptr(name.c_str());
	std::string path(basePathPtr);
	bfree(basePathPtr);

	// Write crash report content to crash dump file
	write_file_content(path, s_crashDumpFromObs.c_str());

	// Send event report to analytics service.
	StreamElementsGlobalStateManager::GetInstance()
		->GetAnalyticsEventsManager()
		->trackSynchronousEvent(
			"OBS Studio Crashed",
			json11::Json::object{{"crashReportText",
					      s_crashDumpFromObs.c_str()}});
	if (s_insideExceptionFilter == 0) {
		int ret = MessageBoxA(NULL, CRASH_MESSAGE, "OBS has crashed!",
				      MB_YESNO | MB_ICONERROR | MB_TASKMODAL);

		if (ret == IDYES) {
			size_t len = s_crashDumpFromObs.size();

			HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
			memcpy(GlobalLock(mem), s_crashDumpFromObs.c_str(),
			       len);
			GlobalUnlock(mem);

			OpenClipboard(0);
			EmptyClipboard();
			SetClipboardData(CF_TEXT, mem);
			CloseClipboard();
		}

		exit(-1);
	}

	UNUSED_PARAMETER(param);
#else
	null_crash_handler(format, args, param);
#endif
}

/* ================================================================= */
#ifdef _WIN32
static inline void AddObsConfigurationFiles()
{
	const size_t BUF_LEN = 2048;
	wchar_t *pathBuffer = new wchar_t[BUF_LEN];

	if (!::GetTempPathW(BUF_LEN, pathBuffer)) {
		delete[] pathBuffer;
		return;
	}

	std::wstring wtempBufPath(pathBuffer);

	if (0 == ::GetTempFileNameW(wtempBufPath.c_str(),
				    L"obs-live-error-report-data", 0,
				    pathBuffer)) {
		delete[] pathBuffer;
		return;
	}

	wtempBufPath = pathBuffer;
	wtempBufPath += L".zip";

	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	std::string tempBufPath = myconv.to_bytes(wtempBufPath);

	char programDataPathBuf[BUF_LEN];
	int ret = os_get_config_path(programDataPathBuf, BUF_LEN, "obs-studio");

	if (ret <= 0) {
		delete[] pathBuffer;
		return;
	}

	delete[] pathBuffer;
	pathBuffer = nullptr;

	std::wstring obsDataPath = QString(programDataPathBuf).toStdWString();

	zip_t *zip = zip_open(tempBufPath.c_str(), 9, 'w');

	if (!zip) {
		return;
	}

	auto addBufferToZip = [&](BYTE *buf, size_t bufLen,
				  std::wstring zipPath) {
		zip_entry_open(zip, myconv.to_bytes(zipPath).c_str());

		zip_entry_write(zip, buf, bufLen);

		zip_entry_close(zip);
	};

	auto addLinesBufferToZip = [&](std::vector<std::string> &lines,
				       std::wstring zipPath) {
		zip_entry_open(zip, myconv.to_bytes(zipPath).c_str());

		for (auto line : lines) {
			zip_entry_write(zip, line.c_str(), line.size());
			zip_entry_write(zip, "\r\n", 2);
		}

		zip_entry_close(zip);
	};

	auto addCefValueToZip = [&](CefRefPtr<CefValue> &input,
				    std::wstring zipPath) {
		std::string buf = myconv.to_bytes(
			CefWriteJSON(input, JSON_WRITER_PRETTY_PRINT)
				.ToWString());

		zip_entry_open(zip, myconv.to_bytes(zipPath).c_str());

		zip_entry_write(zip, buf.c_str(), buf.size());

		zip_entry_close(zip);
	};

	auto addFileToZip = [&](std::wstring localPath, std::wstring zipPath) {
		int fd = _wsopen(localPath.c_str(), _O_RDONLY | _O_BINARY,
				 _SH_DENYNO, 0 /*_S_IREAD | _S_IWRITE*/);

		if (-1 != fd) {
			size_t BUF_LEN = 32768;

			BYTE *buf = new BYTE[BUF_LEN];

			zip_entry_open(zip, myconv.to_bytes(zipPath).c_str());

			int read = _read(fd, buf, BUF_LEN);
			while (read > 0) {
				if (0 != zip_entry_write(zip, buf, read)) {
					break;
				}

				read = _read(fd, buf, BUF_LEN);
			}

			zip_entry_close(zip);

			delete[] buf;

			_close(fd);
		} else {
			// Failed opening file for reading
			//
			// This is a crash handler: you can't really do anything
			// here to mitigate.
		}
	};

	auto addWindowCaptureToZip = [&](const HWND &hWnd, int nBitCount,
					 std::wstring zipPath) {
		//calculate the number of color indexes in the color table
		int nColorTableEntries = -1;
		switch (nBitCount) {
		case 1:
			nColorTableEntries = 2;
			break;
		case 4:
			nColorTableEntries = 16;
			break;
		case 8:
			nColorTableEntries = 256;
			break;
		case 16:
		case 24:
		case 32:
			nColorTableEntries = 0;
			break;
		default:
			nColorTableEntries = -1;
			break;
		}

		if (nColorTableEntries == -1) {
			// printf("bad bits-per-pixel argument\n");
			return false;
		}

		HDC hDC = GetDC(hWnd);
		HDC hMemDC = CreateCompatibleDC(hDC);

		int nWidth = 0;
		int nHeight = 0;

		if (hWnd != HWND_DESKTOP) {
			RECT rect;
			GetClientRect(hWnd, &rect);
			nWidth = rect.right - rect.left;
			nHeight = rect.bottom - rect.top;
		} else {
			nWidth = ::GetSystemMetrics(SM_CXSCREEN);
			nHeight = ::GetSystemMetrics(SM_CYSCREEN);
		}

		HBITMAP hBMP = CreateCompatibleBitmap(hDC, nWidth, nHeight);
		SelectObject(hMemDC, hBMP);
		BitBlt(hMemDC, 0, 0, nWidth, nHeight, hDC, 0, 0, SRCCOPY);

		int nStructLength = sizeof(BITMAPINFOHEADER) +
				    sizeof(RGBQUAD) * nColorTableEntries;
		LPBITMAPINFOHEADER lpBitmapInfoHeader =
			(LPBITMAPINFOHEADER) new char[nStructLength];
		::ZeroMemory(lpBitmapInfoHeader, nStructLength);

		lpBitmapInfoHeader->biSize = sizeof(BITMAPINFOHEADER);
		lpBitmapInfoHeader->biWidth = nWidth;
		lpBitmapInfoHeader->biHeight = nHeight;
		lpBitmapInfoHeader->biPlanes = 1;
		lpBitmapInfoHeader->biBitCount = nBitCount;
		lpBitmapInfoHeader->biCompression = BI_RGB;
		lpBitmapInfoHeader->biXPelsPerMeter = 0;
		lpBitmapInfoHeader->biYPelsPerMeter = 0;
		lpBitmapInfoHeader->biClrUsed = nColorTableEntries;
		lpBitmapInfoHeader->biClrImportant = nColorTableEntries;

		DWORD dwBytes = ((DWORD)nWidth * nBitCount) / 32;
		if (((DWORD)nWidth * nBitCount) % 32) {
			dwBytes++;
		}
		dwBytes *= 4;

		DWORD dwSizeImage = dwBytes * nHeight;
		lpBitmapInfoHeader->biSizeImage = dwSizeImage;

		LPBYTE lpDibBits = 0;
		HBITMAP hBitmap = ::CreateDIBSection(
			hMemDC, (LPBITMAPINFO)lpBitmapInfoHeader,
			DIB_RGB_COLORS, (void **)&lpDibBits, NULL, 0);
		SelectObject(hMemDC, hBitmap);
		BitBlt(hMemDC, 0, 0, nWidth, nHeight, hDC, 0, 0, SRCCOPY);
		ReleaseDC(hWnd, hDC);

		BITMAPFILEHEADER bmfh;
		bmfh.bfType = 0x4d42; // 'BM'
		int nHeaderSize = sizeof(BITMAPINFOHEADER) +
				  sizeof(RGBQUAD) * nColorTableEntries;
		bmfh.bfSize = 0;
		bmfh.bfReserved1 = bmfh.bfReserved2 = 0;
		bmfh.bfOffBits = sizeof(BITMAPFILEHEADER) +
				 sizeof(BITMAPINFOHEADER) +
				 sizeof(RGBQUAD) * nColorTableEntries;

		zip_entry_open(zip, myconv.to_bytes(zipPath).c_str());

		DWORD nColorTableSize = 0;
		if (nBitCount != 24) {
			nColorTableSize = (1ULL << nBitCount) * sizeof(RGBQUAD);
		} else {
			nColorTableSize = 0L;
		}

		zip_entry_write(zip, &bmfh, sizeof(BITMAPFILEHEADER));
		zip_entry_write(zip, lpBitmapInfoHeader, nHeaderSize);

		if (nBitCount < 16) {
			//int nBytesWritten = 0;
			RGBQUAD *rgbTable = new RGBQUAD[nColorTableEntries *
							sizeof(RGBQUAD)];
			//fill RGBQUAD table and write it in file
			for (int i = 0; i < nColorTableEntries; ++i) {
				rgbTable[i].rgbRed = rgbTable[i].rgbGreen =
					rgbTable[i].rgbBlue = i;
				rgbTable[i].rgbReserved = 0;

				zip_entry_write(zip, &rgbTable[i],
						sizeof(RGBQUAD));
			}

			delete[] rgbTable;
		}

		zip_entry_write(zip, lpDibBits, dwSizeImage);

		zip_entry_close(zip);

		::DeleteObject(hBMP);
		::DeleteObject(hBitmap);
		delete[] lpBitmapInfoHeader;

		return true;
	};

	std::string package_manifest = "generator=crash_handler\nversion=4\n";
	addBufferToZip((BYTE *)package_manifest.c_str(),
		       package_manifest.size(), L"manifest.ini");

	addBufferToZip((BYTE *)s_crashDumpFromObs.c_str(),
		       s_crashDumpFromObs.size(),
		       L"obs-studio/crashes/crash.log");

	// Add window capture
	addWindowCaptureToZip(
		(HWND)StreamElementsGlobalStateManager::GetInstance()
			->mainWindow()
			->winId(),
		24, L"obs-main-window.bmp");

	std::map<std::wstring, std::wstring> local_to_zip_files_map;

	// Collect files
	std::vector<std::wstring> blacklist = {
		L"plugin_config/obs-streamelements/obs-streamelements-update.exe",
		L"plugin_config/obs-browser/cache/",
		L"plugin_config/obs-browser/blob_storage/",
		L"plugin_config/obs-browser/code cache/",
		L"plugin_config/obs-browser/gpucache/",
		L"plugin_config/obs-browser/visited links/",
		L"plugin_config/obs-browser/transportsecurity/",
		L"plugin_config/obs-browser/videodecodestats/",
		L"plugin_config/obs-browser/session storage/",
		L"plugin_config/obs-browser/service worker/",
		L"plugin_config/obs-browser/pepper data/",
		L"plugin_config/obs-browser/indexeddb/",
		L"plugin_config/obs-browser/file system/",
		L"plugin_config/obs-browser/databases/",
		L"plugin_config/obs-browser/obs-browser-streamelements.ini.bak",
		L"plugin_config/obs-browser/cef.",
		L"plugin_config/obs-browser/obs_profile_cookies/",
		L"updates/",
		L"profiler_data/",
		L"obslive_restored_files/",
		L"plugin_config/obs-browser/streamelements_restored_files/",
		L"crashes/"};

	// Collect all files
	for (auto &i :
	     std::experimental::filesystem::recursive_directory_iterator(
		     programDataPathBuf)) {
		if (!std::experimental::filesystem::is_directory(i.path())) {
			std::wstring local_path = i.path().c_str();
			std::wstring zip_path =
				local_path.substr(obsDataPath.size() + 1);

			std::wstring zip_path_lcase = zip_path;
			std::transform(zip_path_lcase.begin(),
				       zip_path_lcase.end(),
				       zip_path_lcase.begin(), ::towlower);
			std::transform(zip_path_lcase.begin(),
				       zip_path_lcase.end(),
				       zip_path_lcase.begin(), [](wchar_t ch) {
					       return ch == L'\\' ? L'/' : ch;
				       });

			bool accept = true;
			for (auto item : blacklist) {
				if (zip_path_lcase.size() >= item.size()) {
					if (zip_path_lcase.substr(
						    0, item.size()) == item) {
						accept = false;

						break;
					}
				}
			}

			if (accept) {
				local_to_zip_files_map[local_path] =
					L"obs-studio\\" + zip_path;
			}
		}
	}

	for (auto item : local_to_zip_files_map) {
		addFileToZip(item.first, item.second);
	}

	{
		CefRefPtr<CefValue> basicInfo = CefValue::Create();
		CefRefPtr<CefDictionaryValue> d = CefDictionaryValue::Create();
		basicInfo->SetDictionary(d);

		d->SetString("obsVersion", obs_get_version_string());
		d->SetString("cefVersion", GetCefVersionString());
		d->SetString("cefApiHash", GetCefPlatformApiHash());
#ifdef _WIN32
		d->SetString("platform", "windows");
#elif APPLE
		d->SetString("platform", "macos");
#elif LINUX
		d->SetString("platform", "linux");
#else
		d->SetString("platform", "other");
#endif
		d->SetString("streamelementsPluginVersion",
			     GetStreamElementsPluginVersionString());
#ifdef _WIN64
		d->SetString("platformArch", "64bit");
#else
		d->SetString("platformArch", "32bit");
#endif
		d->SetString("machineUniqueId", GetComputerSystemUniqueId());

		addCefValueToZip(basicInfo, L"system\\basic.json");
	}

	{
		CefRefPtr<CefValue> sysHardwareInfo = CefValue::Create();

		SerializeSystemHardwareProperties(sysHardwareInfo);

		addCefValueToZip(sysHardwareInfo, L"system\\hardware.json");
	}

	{
		CefRefPtr<CefValue> sysMemoryInfo = CefValue::Create();

		SerializeSystemMemoryUsage(sysMemoryInfo);

		addCefValueToZip(sysMemoryInfo, L"system\\memory.json");
	}

	{
		// Histogram CPU & memory usage (past hour, 1 minute intervals)

		auto cpuUsageHistory =
			StreamElementsGlobalStateManager::GetInstance()
				->GetPerformanceHistoryTracker()
				->getCpuUsageSnapshot();

		auto memoryUsageHistory =
			StreamElementsGlobalStateManager::GetInstance()
				->GetPerformanceHistoryTracker()
				->getMemoryUsageSnapshot();

		char lineBuf[512];

		{
			std::vector<std::string> lines;

			lines.push_back("totalSeconds,busySeconds,idleSeconds");
			for (auto item : cpuUsageHistory) {
				sprintf(lineBuf, "%1.2Lf,%1.2Lf,%1.2Lf",
					item.totalSeconds, item.busySeconds,
					item.idleSeconds);

				lines.push_back(lineBuf);
			}

			addLinesBufferToZip(lines,
					    L"system\\usage_history_cpu.csv");
		}

		{
			std::vector<std::string> lines;

			lines.push_back("totalSeconds,memoryUsedPercentage");

			size_t index = 0;
			for (auto item : memoryUsageHistory) {
				if (index < cpuUsageHistory.size()) {
					auto totalSec = cpuUsageHistory[index]
								.totalSeconds;

					sprintf(lineBuf, "%1.2Lf,%d", totalSec,
						item.dwMemoryLoad // % Used
					);
				} else {
					sprintf(lineBuf, "%1.2Lf,%d", 0.0,
						item.dwMemoryLoad // % Used
					);
				}

				lines.push_back(lineBuf);

				++index;
			}

			addLinesBufferToZip(
				lines, L"system\\usage_history_memory.csv");
		}
	}

	zip_close(zip);

	s_mdSender->sendAdditionalFile(wtempBufPath.c_str());
}

/* ================================================================= */

static bool BugSplatExceptionCallback(UINT nCode, LPVOID lpVal1, LPVOID lpVal2)
{
	UNUSED_PARAMETER(lpVal1);
	UNUSED_PARAMETER(lpVal2);

	switch (nCode) {
	case MDSCB_EXCEPTIONCODE:
		//EXCEPTION_RECORD *p = (EXCEPTION_RECORD *)lpVal1;
		//DWORD code = p ? p->ExceptionCode : 0;

		/*
		std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
		s_mdSender->setDefaultUserDescription(
			myconv.from_bytes(std::regex_replace(
						  s_crashDumpFromStackWalker,
						  std::regex("\n"), "\\n"))
				.c_str());
		*/

		AddObsConfigurationFiles();
		break;
	}

	return false;
}

/* ================================================================= */

static LONG CALLBACK CustomExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo)
{
	if (pExceptionInfo->ExceptionRecord->ExceptionCode ==
	    EXCEPTION_STACK_OVERFLOW) {
		static ULONG stack_size = 0L;
		if (SetThreadStackGuarantee(&stack_size)) {
			stack_size += 1024 * 32; // add another 32KB

			SetThreadStackGuarantee(&stack_size);
		}
	}

	s_stackWalker->ShowCallstack(::GetCurrentThread(),
				     pExceptionInfo->ContextRecord);

	if (InterlockedIncrement(&s_insideExceptionFilter) == 1L) {
		if (s_prevExceptionFilter) {
			s_prevExceptionFilter(pExceptionInfo);
		}

		if (s_mdSender && (s_stackWalker->hasMatchModuleOfInterest)) {
			s_mdSender->unhandledExceptionHandler(pExceptionInfo);
		}

		int ret = MessageBoxA(NULL, CRASH_MESSAGE, "OBS has crashed!",
				      MB_YESNO | MB_ICONERROR | MB_TASKMODAL);

		if (ret == IDYES) {
			size_t len = s_crashDumpFromObs.size();

			HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
			memcpy(GlobalLock(mem), s_crashDumpFromObs.c_str(),
			       len);
			GlobalUnlock(mem);

			OpenClipboard(0);
			EmptyClipboard();
			SetClipboardData(CF_TEXT, mem);
			CloseClipboard();
		}

		exit(-1);
	}

	InterlockedDecrement(&s_insideExceptionFilter);

	return EXCEPTION_CONTINUE_SEARCH;
}

#endif
/* ================================================================= */

StreamElementsCrashHandler::StreamElementsCrashHandler()
{
#ifdef _WIN32
	base_set_crash_handler(main_crash_handler, this);
#else
	base_set_crash_handler(null_crash_handler, this);
#endif
#ifdef _WIN32
	if (IsDebuggerPresent()) {
		return;
	}

	s_crashDumpFromObs.reserve(1024 * 16);
	s_crashDumpFromStackWalker.reserve(1024 * 16);

	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	std::wstring plugin_version =
		myconv.from_bytes(GetStreamElementsPluginVersionString());
	std::wstring obs_version = myconv.from_bytes(obs_get_version_string());

	std::wstring app_id = std::wstring(L"OBS ") + obs_version;
#ifdef _WIN64
	app_id += L" (64bit)";
#else
	app_id += L" (32bit)";
#endif

	s_stackWalker = new MyStackWalker(StackWalker::RetrieveSymbol |
					  StackWalker::RetrieveLine |
					  StackWalker::RetrieveModuleInfo);

	s_stackWalker->modulesOfInterest.push_back("obs-browser");
	s_stackWalker->modulesOfInterest.push_back("libobs");
	s_stackWalker->modulesOfInterest.push_back("obs32");
	s_stackWalker->modulesOfInterest.push_back("obs64");

	s_mdSender = new MiniDmpSender(
		L"OBS_Live", L"obs-browser", plugin_version.c_str(),
		app_id.c_str(),
		MDSF_CUSTOMEXCEPTIONFILTER | MDSF_USEGUARDMEMORY |
			MDSF_LOGFILE | MDSF_LOG_VERBOSE | MDSF_NONINTERACTIVE);

	// Set optional default values for user, email, and user description of the crash.
	s_mdSender->setDefaultUserName(L"Unknown");
	s_mdSender->setDefaultUserEmail(L"anonymous@user.com");
	s_mdSender->setDefaultUserDescription(L"");
	s_mdSender->setGuardByteBufferSize(1024 * 1024); // Allocate 1MB of guard buffer

	s_mdSender->setCallback(BugSplatExceptionCallback);

	s_prevExceptionFilter =
		SetUnhandledExceptionFilter(CustomExceptionFilter);
#endif
}

StreamElementsCrashHandler::~StreamElementsCrashHandler()
{
#ifdef _WIN32
	if (s_prevExceptionFilter) {
	// Unbind our exception filter
		SetUnhandledExceptionFilter(s_prevExceptionFilter);

		s_prevExceptionFilter = nullptr;
	}
#endif

	base_set_crash_handler(null_crash_handler, nullptr);
}

