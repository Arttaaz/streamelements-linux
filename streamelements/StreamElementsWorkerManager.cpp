#include "StreamElementsWorkerManager.hpp"
#include "StreamElementsApiMessageHandler.hpp"
#include "StreamElementsGlobalStateManager.hpp"

#include <QUuid>
#include <QWidget>

#include <include/cef_parser.h> // CefParseJSON, CefWriteJSON

class BrowserTask : public CefTask {
public:
	std::function<void()> task;

	inline BrowserTask(std::function<void()> task_) : task(task_) {}
	virtual void Execute() override { task(); }

	IMPLEMENT_REFCOUNTING(BrowserTask);
};

static bool QueueCEFTask(std::function<void()> task)
{
	return CefPostTask(TID_UI,
			   CefRefPtr<BrowserTask>(new BrowserTask(task)));
}

/* ========================================================================= */

class StreamElementsWorkerManager::StreamElementsWorker : public QWidget {
public:
	StreamElementsWorker(std::string id, std::string content,
			     std::string url,
			     std::string executeJavascriptOnLoad)
		: QWidget(),
		  m_content(content),
		  m_url(url),
		  m_executeJavascriptOnLoad(executeJavascriptOnLoad),
		  m_cef_browser(nullptr)
	{
		cef_window_handle_t windowHandle = (cef_window_handle_t)winId();

		QueueCEFTask([this, windowHandle, id, content, url]() {
			if (!!m_cef_browser.get()) {
				return;
			}

			CefRect clientRect;
			clientRect.x = 0;
			clientRect.y = 0;
			clientRect.width = 1920;
			clientRect.height = 1080;

			// CEF window attributes
			CefWindowInfo windowInfo;
			windowInfo.width = 1920;
			windowInfo.height = 1080;
			windowInfo.windowless_rendering_enabled = true;
			//windowInfo.SetAsChild(windowHandle, clientRect);

			CefBrowserSettings cefBrowserSettings;

			cefBrowserSettings.Reset();
			cefBrowserSettings.javascript_close_windows =
				STATE_DISABLED;
			cefBrowserSettings.local_storage = STATE_ENABLED;
			cefBrowserSettings.databases = STATE_ENABLED;
			cefBrowserSettings.web_security = STATE_ENABLED;
			cefBrowserSettings.webgl = STATE_ENABLED;

			CefRefPtr<StreamElementsCefClient> cefClient =
				new StreamElementsCefClient(
					m_executeJavascriptOnLoad,
					new StreamElementsApiMessageHandler(),
					nullptr,
					StreamElementsMessageBus::DEST_WORKER);

			cefClient->SetContainerId(id);
			cefClient->SetLocationArea("worker");

			m_cef_browser = CefBrowserHost::CreateBrowserSync(
				windowInfo, cefClient, "about:blank",
				cefBrowserSettings,
#if CHROME_VERSION_BUILD >= 3770
				CefRefPtr<CefDictionaryValue>(),
#endif
				StreamElementsGlobalStateManager::GetInstance()
					->GetCookieManager()
					->GetCefRequestContext());

			m_cef_browser->GetMainFrame()->LoadString(content,
								   url);
		});
	}

	~StreamElementsWorker()
	{
		if (m_cef_browser.get()) {
#ifdef _WIN32
			// Detach browser to prevent WM_CLOSE event from being sent
			// from CEF to the parent window.
			::SetParent(m_cef_browser->GetHost()->GetWindowHandle(),
				    0L);
#endif
			m_cef_browser->GetHost()->CloseBrowser(true);
			m_cef_browser = NULL;
		}
	}

	std::string GetUrl() { return m_url; }
	std::string GetContent() { return m_content; }
	std::string GetExecuteJavaScriptOnLoad() { return m_executeJavascriptOnLoad; }

private:
	std::string m_content;
	std::string m_url;
	std::string m_executeJavascriptOnLoad;
	CefRefPtr<CefBrowser> m_cef_browser;
};

StreamElementsWorkerManager::StreamElementsWorkerManager() {}

StreamElementsWorkerManager::~StreamElementsWorkerManager() {}

void StreamElementsWorkerManager::OnObsExit() {}

void StreamElementsWorkerManager::RemoveAll()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	while (m_items.begin() != m_items.end()) {
		Remove(m_items.begin()->first);
	}
}

std::string
StreamElementsWorkerManager::Add(std::string requestedId, std::string content,
				 std::string url,
				 std::string executeJavascriptOnLoad)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	std::string id = requestedId;

	if (!id.size() || m_items.count(id)) {
		id = QUuid::createUuid().toString().toStdString();
	}

	m_items[id] = new StreamElementsWorker(id, content, url,
					       executeJavascriptOnLoad);

	return id;
}

void StreamElementsWorkerManager::Remove(std::string id)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (m_items.count(id)) {
		StreamElementsWorker *item = m_items[id];

		m_items.erase(id);

		delete item;
	}
}

std::string StreamElementsWorkerManager::GetContent(std::string id)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (m_items.count(id)) {
		return m_items[id]->GetContent();
	}

	return "";
}

void StreamElementsWorkerManager::GetIdentifiers(
	std::vector<std::string> &result)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	for (auto it = m_items.begin(); it != m_items.end(); ++it) {
		result.push_back(it->first);
	}
}

void StreamElementsWorkerManager::Serialize(CefRefPtr<CefValue> &output)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
	output->SetDictionary(root);

	for (auto it = m_items.begin(); it != m_items.end(); ++it) {
		CefRefPtr<CefValue> itemValue = CefValue::Create();
		CefRefPtr<CefDictionaryValue> item =
			CefDictionaryValue::Create();
		itemValue->SetDictionary(item);

		item->SetString("id", it->first);
		item->SetString("content", it->second->GetContent());
		item->SetString("url", it->second->GetUrl());
		item->SetString("executeJavaScriptOnLoad",
				it->second->GetExecuteJavaScriptOnLoad());

		root->SetValue(it->first, itemValue);
	}
}

void StreamElementsWorkerManager::Deserialize(CefRefPtr<CefValue> &input)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (!!input.get() && input->GetType() == VTYPE_DICTIONARY) {
		CefRefPtr<CefDictionaryValue> root = input->GetDictionary();

		CefDictionaryValue::KeyList keys;
		if (root->GetKeys(keys)) {
			for (auto key : keys) {
				std::string id = key.ToString();

				CefRefPtr<CefDictionaryValue> dict =
					root->GetDictionary(key);

				if (!!dict.get() && dict->HasKey("content") &&
				    dict->HasKey("url")) {
					std::string content =
						dict->GetString("content");
					std::string url =
						dict->GetString("url");

					std::string executeJavaScriptOnLoad =
						"";

					if (dict->HasKey(
						    "executeJavaScriptOnLoad") &&
					    dict->GetType(
						    "executeJavaScriptOnLoad") ==
						    VTYPE_STRING) {
						executeJavaScriptOnLoad =
							dict->GetString(
								    "executeJavaScriptOnLoad")
								.ToString();
					}

					Add(id, content, url,
					    executeJavaScriptOnLoad);
				}
			}
		}
	}
}

bool StreamElementsWorkerManager::SerializeOne(std::string id,
					       CefRefPtr<CefValue> &output)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
	output->SetDictionary(item);

	item->SetString("id", id);
	item->SetString("content", m_items[id]->GetContent());
	item->SetString("url", m_items[id]->GetUrl());
	item->SetString("executeJavaScriptOnLoad",
			m_items[id]->GetExecuteJavaScriptOnLoad());

	return true;
}

std::string
StreamElementsWorkerManager::DeserializeOne(CefRefPtr<CefValue> input)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (input.get() && input->GetType() == VTYPE_DICTIONARY) {
		CefRefPtr<CefDictionaryValue> dict = input->GetDictionary();

		if (!!dict.get() && dict->HasKey("content") &&
		    dict->HasKey("url")) {
			std::string id =
				dict->HasKey("id") ? dict->GetString("id") : "";
			std::string content = dict->GetString("content");
			std::string url = dict->GetString("url");

					std::string executeJavaScriptOnLoad = "";

			if (dict->HasKey("executeJavaScriptOnLoad") &&
			    dict->GetType("executeJavaScriptOnLoad") ==
				    VTYPE_STRING) {
				executeJavaScriptOnLoad =
					dict->GetString(
						    "executeJavaScriptOnLoad")
						.ToString();
			}

			return Add(id, content, url, executeJavaScriptOnLoad);
		}
	}

	return "";
}
