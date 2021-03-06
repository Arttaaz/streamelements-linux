#include "StreamElementsCefClient.hpp"
#include "StreamElementsUtils.hpp"
#include "StreamElementsGlobalStateManager.hpp"
#include "StreamElementsMessageBus.hpp"
#include "StreamElementsFileSystemMapper.hpp"
#include "base64/base64.hpp"
#include "json11/json11.hpp"
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <include/cef_parser.h>     // CefParseJSON, CefWriteJSON
#include <include/cef_urlrequest.h> // CefURLRequestClient
#include <include/capi/views/cef_window_capi.h>
#include <regex>
#include <sstream>
#include <algorithm>

#include <QWindow>
#include <QIcon>
#include <QWidget>
#include <QFile>

static std::recursive_mutex s_browsers_mutex;
static std::vector<CefRefPtr<CefBrowser>> s_browsers;

/* ========================================================================= */

static bool SetWindowIconFromBuffer(cef_window_handle_t windowHandle,
				    void *buffer, size_t buffer_len)
{
#ifdef _WIN32
	size_t offset = ::LookupIconIdFromDirectoryEx((PBYTE)buffer, TRUE, 0, 0,
						      LR_DEFAULTCOLOR);

	if (offset) {
		size_t size = buffer_len - offset;

		HICON hIcon = ::CreateIconFromResourceEx((PBYTE)buffer + offset,
							 (DWORD)size, TRUE,
							 0x00030000, 0, 0,
							 LR_SHARED);

		if (hIcon) {
			::SendMessage(windowHandle, WM_SETICON, ICON_BIG,
				      (LPARAM)hIcon);
			::SendMessage(windowHandle, WM_SETICON, ICON_SMALL,
				      (LPARAM)hIcon);

			return true;
		}
	}
	return false;
#else
	//windowHandle.set_window_icon(buffer);
	//windowHandle.set_window_app_icon(buffer);
	return true;
#endif
}

static bool SetWindowIconFromResource(cef_window_handle_t windowHandle,
				      QString &resource)
{
	QFile file(resource);

	if (file.open(QIODevice::ReadOnly)) {
		QByteArray data = file.readAll();

		return SetWindowIconFromBuffer(windowHandle, data.begin(),
					       data.size());
	}

	return false;
}

static bool SetWindowDefaultIcon(cef_window_handle_t windowHandle)
{
	QString icon(":/images/icon.ico");

	return SetWindowIconFromResource(windowHandle, icon);
}

/* ========================================================================= */

using namespace json11;

/* ========================================================================= */

#define CEF_REQUIRE_UI_THREAD() DCHECK(CefCurrentlyOn(TID_UI));
#define CEF_REQUIRE_IO_THREAD() DCHECK(CefCurrentlyOn(TID_IO));
#define CEF_REQUIRE_FILE_THREAD() DCHECK(CefCurrentlyOn(TID_FILE));
#define CEF_REQUIRE_RENDERER_THREAD() DCHECK(CefCurrentlyOn(TID_RENDERER));

/* ========================================================================= */

static long s_cefClientId = 0;

StreamElementsCefClient::StreamElementsCefClient(
	std::string executeJavaScriptCodeOnLoad,
	CefRefPtr<StreamElementsBrowserMessageHandler> messageHandler,
	CefRefPtr<StreamElementsCefClientEventHandler> eventHandler,
	StreamElementsMessageBus::message_destination_filter_flags_t msgDestType)
	: m_executeJavaScriptCodeOnLoad(executeJavaScriptCodeOnLoad),
	  m_messageHandler(messageHandler),
	  m_eventHandler(eventHandler),
	  m_msgDestType(msgDestType),
	  m_cefClientId(os_atomic_inc_long(&s_cefClientId))
{
	blog(LOG_INFO, "obs-browser[%lu]: StreamElementsCefClient: initialized",
	     m_cefClientId);
}

StreamElementsCefClient::~StreamElementsCefClient()
{
	blog(LOG_INFO, "obs-browser[%lu]: StreamElementsCefClient: destroyed",
	     m_cefClientId);
}

/* ========================================================================= */

static std::string sanitize_url(std::string input)
{
	std::string result = input;

	if (result.size() > 5 && result.substr(0, 5) == "data:") {
		result = "data:HIDDEN";
	} else {
		size_t pos = result.find('#');
		if (pos >= 0) {
			result = result.substr(0, pos);
		}
	}

	return result;
}

/* ========================================================================= */

void StreamElementsCefClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
					  CefRefPtr<CefFrame> frame,
					  TransitionType transition_type)
{
	char buf[16];

	blog(LOG_INFO,
	     "obs-browser[%lu]: start loading %s frame url '%s' (transition_type: 0xSTUB)",
	     m_cefClientId, frame->IsMain() ? "main" : "child",
	     sanitize_url(frame->GetURL().ToString()).c_str()
	     /*itoa((int)transition_type, buf, 16)*/);
}

void StreamElementsCefClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
					CefRefPtr<CefFrame> frame,
					int httpStatusCode)
{
#ifdef _WIN32
	cef_window_handle_t hWnd = browser->GetHost()->GetWindowHandle();

	RECT rect;
	if (::GetWindowRect(hWnd, &rect)) {
		LONG width = rect.right - rect.left;
		LONG height = rect.bottom - rect.top;

		::SetWindowPos(hWnd, nullptr, 0, 0, width, height,
			       SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER);

		::SendMessage(hWnd, WM_SIZE, 0, MAKELPARAM(width, height));
	}
#endif

	blog(LOG_INFO,
	     "obs-browser[%lu]: completed loading %s frame url '%s' (HTTP status code: %d)",
	     m_cefClientId, frame->IsMain() ? "main" : "child",
	     sanitize_url(frame->GetURL().ToString()).c_str(), httpStatusCode);

	if (m_executeJavaScriptCodeOnLoad.empty() || !frame->IsMain()) {
		return;
	}

	frame->ExecuteJavaScript(CefString(m_executeJavaScriptCodeOnLoad),
				 frame->GetURL(), 0);
}

void StreamElementsCefClient::OnLoadError(CefRefPtr<CefBrowser> browser,
					  CefRefPtr<CefFrame> frame,
					  ErrorCode errorCode,
					  const CefString &errorText,
					  const CefString &failedUrl)
{
	if (!frame)
		return;

	blog(LOG_WARNING,
	     "obs-browser[%lu]: error loading %s frame url '%s': %s (%d)",
	     m_cefClientId, frame->IsMain() ? "main" : "child",
	     sanitize_url(failedUrl.ToString()).c_str(),
	     errorText.size() ? errorText.ToString().c_str()
			      : "Unknown error code",
	     (int)errorCode);

	if (errorCode == ERR_ABORTED) {
		// Don't display an error for downloaded files and
		// pages which have been left while loading.
		// (loading aborted)
		return;
	}

	if (!frame->IsMain()) {
		return;
	}

	std::string htmlString = LoadResourceString(":/html/error.html");

	if (!htmlString.size()) {
		// Default
		htmlString =
			"<html><body><h1>error page</h1><p>${error.code}</p><p>${error.url}</p></body></html>";
	}

	std::stringstream error;
	if (errorText.size()) {
		error << errorText.ToString();
	} else {
		error << "UNKNOWN"
		      << " (" << (int)errorCode << ")";
	}

	htmlString = std::regex_replace(
		htmlString, std::regex("\\$\\{error.code\\}"), error.str());
	htmlString = std::regex_replace(
		htmlString, std::regex("\\$\\{error.text\\}"), error.str());
	htmlString = std::regex_replace(htmlString,
					std::regex("\\$\\{error.url\\}"),
					failedUrl.ToString());

	frame->LoadString(htmlString, failedUrl);
}

void StreamElementsCefClient::OnLoadingStateChange(
	CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack,
	bool canGoForward)
{
	if (!m_eventHandler.get()) {
		return;
	}

	m_eventHandler->OnLoadingStateChange(browser, isLoading, canGoBack,
					     canGoForward);
}

/* ========================================================================= */

bool StreamElementsCefClient::OnProcessMessageReceived(
	CefRefPtr<CefBrowser> browser,
#if CHROME_VERSION_BUILD >= 3770
	CefRefPtr<CefFrame> frame,
#endif
	CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	const std::string &name = message->GetName();
	Json json;

	if (name == "getCurrentScene") {
		json = Json::object{};
	} else if (name == "getStatus") {
		json = Json::object{
			{"recording", obs_frontend_recording_active()},
			{"streaming", obs_frontend_streaming_active()},
			{"replaybuffer", obs_frontend_replay_buffer_active()}};

	}
#if CHROME_VERSION_BUILD >= 3770
	else if (m_messageHandler.get() &&
		 m_messageHandler->OnProcessMessageReceived(
			 browser, frame, source_process, message,
			 m_cefClientId)) {
		return true;
	}

#else
	else if (m_messageHandler.get() &&
		 m_messageHandler->OnProcessMessageReceived(
			 browser, source_process, message, m_cefClientId)) {
		return true;
	}
#endif
	else {

		return false;
	}

	CefRefPtr<CefProcessMessage> msg =
		CefProcessMessage::Create("executeCallback");

	CefRefPtr<CefListValue> args = msg->GetArgumentList();
	args->SetInt(0, message->GetArgumentList()->GetInt(0));
	args->SetString(1, json.dump());

	SendBrowserProcessMessage(browser, PID_RENDERER, msg);

	return true;
}

void StreamElementsCefClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
					    const CefString &title)
{
	if (!browser || !browser->GetHost() || title.empty()) {
		return;
	}

	//
	// Do not use QWindow::fromWinId here
	//
	// http://doc.qt.io/qt-5/qwindow.html#fromWinId
	// Note: The resulting QWindow should not be used to
	//       manipulate the underlying native window (besides re-parenting),
	//       or to observe state changes of the native window. Any support
	//       for these kind of operations is incidental, highly platform
	//       dependent and untested.
	//

#ifdef _WIN32
	SetWindowTextW(browser->GetHost()->GetWindowHandle(),
		       title.ToWString().c_str());
#endif
}

void StreamElementsCefClient::OnFaviconURLChange(
	CefRefPtr<CefBrowser> browser, const std::vector<CefString> &icon_urls)
{
#ifdef _WIN32
	UNREFERENCED_PARAMETER(browser);
	UNREFERENCED_PARAMETER(icon_urls);
#else
	browser;
	icon_urls;
#endif
}

bool StreamElementsCefClient::OnConsoleMessage(CefRefPtr<CefBrowser>,
#if CHROME_VERSION_BUILD >= 3282
					       cef_log_severity_t level,
#endif
					       const CefString &message,
					       const CefString &source,
					       int line)
{
	blog(LOG_INFO, "obs-browser[%lu]: CONSOLE: %s (source: %s:%d)",
	     m_cefClientId, message.ToString().c_str(),
	     source.ToString().c_str(), line);

	return false;
}

void StreamElementsCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	SetWindowDefaultIcon(browser->GetHost()->GetWindowHandle());

	{
		std::lock_guard<std::recursive_mutex> guard(s_browsers_mutex);

		s_browsers.push_back(browser);

		StreamElementsMessageBus::GetInstance()->AddBrowserListener(
			browser, m_msgDestType);
	}
}

void StreamElementsCefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	std::lock_guard<std::recursive_mutex> guard(s_browsers_mutex);

	StreamElementsMessageBus::GetInstance()->RemoveBrowserListener(browser);

	auto index =
		std::find(s_browsers.begin(), s_browsers.end(), browser.get());

	if (index != s_browsers.end()) {
		s_browsers.erase(index);
	}
}

void StreamElementsCefClient::DispatchJSEvent(std::string event,
					      std::string eventArgsJson)
{
	std::lock_guard<std::recursive_mutex> guard(s_browsers_mutex);

	for (CefRefPtr<CefBrowser> browser : s_browsers) {
		CefRefPtr<CefProcessMessage> msg =
			CefProcessMessage::Create("DispatchJSEvent");
		CefRefPtr<CefListValue> args = msg->GetArgumentList();

		args->SetString(0, event);
		args->SetString(1, eventArgsJson);
		SendBrowserProcessMessage(browser, PID_RENDERER, msg);
	}
}

void StreamElementsCefClient::DispatchJSEvent(CefRefPtr<CefBrowser> browser,
					      std::string event,
					      std::string eventArgsJson)
{
	if (!browser.get()) {
		return;
	}

	CefRefPtr<CefProcessMessage> msg =
		CefProcessMessage::Create("DispatchJSEvent");
	CefRefPtr<CefListValue> args = msg->GetArgumentList();

	args->SetString(0, event);
	args->SetString(1, eventArgsJson);
	SendBrowserProcessMessage(browser, PID_RENDERER, msg);
}

bool StreamElementsCefClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
					    const CefKeyEvent &event,
					    CefEventHandle os_event,
					    bool *is_keyboard_shortcut)
{
#ifdef _WIN32
	UNREFERENCED_PARAMETER(os_event);
	UNREFERENCED_PARAMETER(is_keyboard_shortcut);
#else
	os_event;
	is_keyboard_shortcut;
#endif

	if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYUP) {
		return false;
	}

	if (event.is_system_key) {
		return false;
	}

	StreamElementsGlobalStateManager *global =
		StreamElementsGlobalStateManager::GetInstance();

	if (!global) {
		return false;
	}

	obs_key_combination_t combo = {0};

	bool pressed = event.type == KEYEVENT_KEYDOWN ||
		       event.type == KEYEVENT_RAWKEYDOWN;

#ifdef _WIN32
	// Bit 30 - the previous key state
	// https://docs.microsoft.com/en-us/windows/desktop/inputdev/wm-syskeydown
	//
	bool repeated = !!((event.native_key_code >> 30) & 1);

	if (pressed && repeated) {
		return false;
	}
#endif

	int virtualKeyCode = event.windows_key_code;

	// Translate virtual key code to OBS key code
	combo.key = obs_key_from_virtual_key(virtualKeyCode);

	struct modifier_map_t {
		unsigned int virtualKey;
		obs_interaction_flags obs;
	};

	const unsigned short FLAG_PRESSED = 0x8000;
	const unsigned short FLAG_TOGGLED = 0x0001;

	// OBS hotkey thread currently supports only Ctrl, Shift, Alt modifiers.
	//
	// We'll align our resolution of modifiers to what OBS supports.
	//
	static const modifier_map_t mods_map_pressed[] = {
		{KBC_SHIFT, INTERACT_SHIFT_KEY},
		//{ VK_LSHIFT, INTERACT_SHIFT_KEY },
		//{ VK_RSHIFT, INTERACT_SHIFT_KEY },

		//{ VK_LCONTROL, INTERACT_CONTROL_KEY },
		//{ VK_RCONTROL, INTERACT_CONTROL_KEY },
		{KBC_CONTROL, INTERACT_CONTROL_KEY},

		{KBC_END, INTERACT_ALT_KEY},
		//{ VK_LMENU, INTERACT_ALT_KEY },
		//{ VK_RMENU, INTERACT_ALT_KEY },

		//{ VK_LBUTTON, INTERACT_MOUSE_LEFT },
		//{ VK_RBUTTON, INTERACT_MOUSE_MIDDLE },
		//{ VK_MBUTTON, INTERACT_MOUSE_RIGHT },

		//{ VK_LWIN, INTERACT_COMMAND_KEY },
		//{ VK_RWIN, INTERACT_COMMAND_KEY }
	};
#ifdef _WIN32
	for (auto map_item : mods_map_pressed) {
		if (map_item.virtualKey != 0 &&
		    map_item.virtualKey != virtualKeyCode) {
			short keyState =
				::GetAsyncKeyState(map_item.virtualKey);

			if (keyState != 0) {
				combo.modifiers = combo.modifiers |
						  map_item.obs;
			}
		}
	}
#endif
	/*
	// Toggled modifiers are not currently supported by OBS hotkey thread.
	//
	static const modifier_map_t mods_map_toggled[] = {
		{ VK_CAPITAL, INTERACT_CAPS_KEY },
		{ VK_NUMLOCK, INTERACT_NUMLOCK_KEY }
		//{ 0, INTERACT_IS_KEY_PAD },
		//{ 0, INTERACT_IS_LEFT },
		//{ 0, INTERACT_IS_RIGHT }
	};

	for (auto map_item : mods_map_toggled) {
		if (map_item.virtualKey != 0) {
			SHORT keyState = ::GetAsyncKeyState(map_item.virtualKey);

			if (!!(keyState & FLAG_TOGGLED == FLAG_TOGGLED)) {
				combo.modifiers = combo.modifiers | map_item.obs;
			}
		}
	}
	*/

	global->GetHotkeyManager()->keyCombinationTriggered(browser, combo,
							    pressed);

	// Keyboard events which occur while CEF browser is in focus
	// are not bubbled up.
	//
	// Send the keystroke to the hotkey processing queue.
	obs_hotkey_inject_event(combo, pressed);

	return false;
}

class BrowserSchemeHandler : public CefResourceHandler {
	std::string fileName;
	std::ifstream inputStream;
	bool isComplete = false;
	int64 length = 0;
	int64 remaining = 0;

public:
	virtual bool ProcessRequest(CefRefPtr<CefRequest> request,
				    CefRefPtr<CefCallback> callback) override;
	virtual void GetResponseHeaders(CefRefPtr<CefResponse> response,
					int64 &response_length,
					CefString &redirectUrl) override;
	virtual bool ReadResponse(void *data_out, int bytes_to_read,
				  int &bytes_read,
				  CefRefPtr<CefCallback> callback) override;
	virtual void Cancel() override;

	IMPLEMENT_REFCOUNTING(BrowserSchemeHandler);
};

CefRefPtr<CefResourceHandler>
StreamElementsCefClient::GetResourceHandler(CefRefPtr<CefBrowser> browser,
					    CefRefPtr<CefFrame> frame,
					    CefRefPtr<CefRequest> request)
{
	///
	// Intercept requests to //absolute/ and locally mapped hosts
	//
	return StreamElementsGlobalStateManager::GetInstance()
		->GetLocalWebFilesServer()
		->GetCefResourceHandler(browser, frame, request);
}
