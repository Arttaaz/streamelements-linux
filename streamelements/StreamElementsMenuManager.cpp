#include "StreamElementsMenuManager.hpp"
#include "StreamElementsGlobalStateManager.hpp"
#include "StreamElementsConfig.hpp"

#include <callback/signal.h>

#include "../cef-headers.hpp"

#include <QObjectList>
#include <QDesktopServices>
#include <QUrl>

#include <algorithm>

StreamElementsMenuManager::StreamElementsMenuManager(QMainWindow *parent)
	: m_mainWindow(parent)
{
	m_auxMenuItems = CefValue::Create();
	m_auxMenuItems->SetNull();

	m_menu = new QMenu("St&reamElements");

	mainWindow()->menuBar()->addMenu(m_menu);

	LoadConfig();
}

StreamElementsMenuManager::~StreamElementsMenuManager()
{
	m_menu->menuAction()->setVisible(false);
	m_menu = nullptr;
}

void StreamElementsMenuManager::Update()
{
	SYNC_ACCESS();

	if (!m_menu)
		return;

	m_menu->clear();

	auto addURL = [this](QString title, QString url) {
		QAction *menu_action = new QAction(title);
		m_menu->addAction(menu_action);

		menu_action->connect(
			menu_action, &QAction::triggered, [this, url] {
				QUrl navigate_url =
					QUrl(url, QUrl::TolerantMode);
				QDesktopServices::openUrl(navigate_url);
			});
	};

	QAction *onboarding_action = new QAction(
		obs_module_text("StreamElements.Action.ForceOnboarding"));
	m_menu->addAction(onboarding_action);
	onboarding_action->connect(onboarding_action, &QAction::triggered, [this] {
		QtPostTask(
			[](void *) -> void {
				StreamElementsGlobalStateManager::GetInstance()
					->Reset(false,
						StreamElementsGlobalStateManager::
							OnBoarding);
			},
			nullptr);
	});

	addURL(obs_module_text("StreamElements.Action.Overlays"),
	       obs_module_text("StreamElements.Action.Overlays.URL"));
	addURL(obs_module_text("StreamElements.Action.GroundControl"),
	       obs_module_text("StreamElements.Action.GroundControl.URL"));
	m_menu->addSeparator();
	{
		StreamElementsGlobalStateManager::GetInstance()
			->GetWidgetManager()
			->EnterCriticalSection();

		std::vector<std::string> widgetIds;
		StreamElementsGlobalStateManager::GetInstance()
			->GetWidgetManager()
			->GetDockBrowserWidgetIdentifiers(widgetIds);

		std::vector<StreamElementsBrowserWidgetManager::
				    DockBrowserWidgetInfo *>
			widgets;
		for (auto id : widgetIds) {
			auto info =
				StreamElementsGlobalStateManager::GetInstance()
					->GetWidgetManager()
					->GetDockBrowserWidgetInfo(id.c_str());

			if (info) {
				widgets.push_back(info);
			}
		}

		std::sort(widgets.begin(), widgets.end(),
			  [](StreamElementsBrowserWidgetManager::
				     DockBrowserWidgetInfo *a,
			     StreamElementsBrowserWidgetManager::
				     DockBrowserWidgetInfo *b) {
				  return a->m_title < b->m_title;
			  });

		StreamElementsGlobalStateManager::GetInstance()
			->GetWidgetManager()
			->LeaveCriticalSection();

		for (auto widget : widgets) {
			// widget->m_visible
			QAction *widget_action =
				new QAction(QString(widget->m_title.c_str()));
			m_menu->addAction(widget_action);

			std::string id = widget->m_id;
			bool isVisible = widget->m_visible;

			widget_action->setCheckable(true);
			widget_action->setChecked(isVisible);

			QObject::connect(widget_action, &QAction::triggered, [this, id, isVisible, widget_action] {
				QDockWidget *dock =
					StreamElementsGlobalStateManager::
						GetInstance()
							->GetWidgetManager()
							->GetDockWidget(
								id.c_str());

				if (dock) {
					if (isVisible) {
						// Hide
						StreamElementsGlobalStateManager::GetInstance()
							->GetAnalyticsEventsManager()
							->trackDockWidgetEvent(
								dock, "Hide",
								json11::Json::object{
									{"actionSource",
									 "Menu"}});
					} else {
						// Show
						StreamElementsGlobalStateManager::GetInstance()
							->GetAnalyticsEventsManager()
							->trackDockWidgetEvent(
								dock, "Show",
								json11::Json::object{
									{"actionSource",
									 "Menu"}});
					}

					dock->setVisible(!isVisible);

					Update();
				}
			});
		}

		for (auto widget : widgets) {
			delete widget;
		}
	}
	m_menu->addSeparator();

	QAction *import_action =
		new QAction(obs_module_text("StreamElements.Action.Import"));
	m_menu->addAction(import_action);
	import_action->connect(import_action, &QAction::triggered, [this] {
		QtPostTask(
			[](void *) -> void {
				StreamElementsGlobalStateManager::GetInstance()
					->Reset(false,
						StreamElementsGlobalStateManager::
							Import);
			},
			nullptr);
	});

	DeserializeMenu(m_auxMenuItems, *m_menu);

	m_menu->addSeparator();

	QAction *check_for_updates_action = new QAction(
		obs_module_text("StreamElements.Action.CheckForUpdates"));
	m_menu->addAction(check_for_updates_action);
	check_for_updates_action->connect(
		check_for_updates_action, &QAction::triggered, [this] {
			calldata_t *cd = calldata_create();
			calldata_set_bool(cd, "allow_downgrade", false);
			calldata_set_bool(cd, "force_install", false);
			calldata_set_bool(cd, "allow_use_last_response", false);

			signal_handler_signal(
				obs_get_signal_handler(),
				"streamelements_request_check_for_updates",
				cd);

			calldata_free(cd);
		});

	m_menu->addSeparator();

	QAction *stop_onboarding_ui = new QAction(
		obs_module_text("StreamElements.Action.StopOnBoardingUI"));
	m_menu->addAction(stop_onboarding_ui);
	stop_onboarding_ui->connect(
		stop_onboarding_ui, &QAction::triggered, [this] {
			StreamElementsGlobalStateManager::GetInstance()
				->SwitchToOBSStudio();
		});

	QAction *uninstall =
		new QAction(obs_module_text("StreamElements.Action.Uninstall"));
	m_menu->addAction(uninstall);
	uninstall->connect(uninstall, &QAction::triggered, [this] {
		StreamElementsGlobalStateManager::GetInstance()
			->UninstallPlugin();
	});

	m_menu->addSeparator();

	QAction *report_issue = new QAction(
		obs_module_text("StreamElements.Action.ReportIssue"));
	m_menu->addAction(report_issue);
	report_issue->connect(report_issue, &QAction::triggered, [this] {
		StreamElementsGlobalStateManager::GetInstance()->ReportIssue();
	});

	addURL(obs_module_text("StreamElements.Action.LiveSupport"),
	       obs_module_text("StreamElements.Action.LiveSupport.URL"));

	m_menu->addSeparator();

	{
		bool isLoggedIn =
			StreamElementsConfig::STARTUP_FLAGS_SIGNED_IN ==
			(StreamElementsConfig::GetInstance()->GetStartupFlags() &
			 StreamElementsConfig::STARTUP_FLAGS_SIGNED_IN);

		QAction *logout_action = new QAction(
			isLoggedIn
				? obs_module_text(
					  "StreamElements.Action.ResetStateSignOut")
				: obs_module_text(
					  "StreamElements.Action.ResetStateSignIn"));
		m_menu->addAction(logout_action);
		logout_action->connect(
			logout_action, &QAction::triggered, [this] {
				QtPostTask(
					[](void *) -> void {
						StreamElementsGlobalStateManager::
							GetInstance()
								->Reset();
					},
					nullptr);
			});
	}
}

bool StreamElementsMenuManager::DeserializeAuxiliaryMenuItems(
	CefRefPtr<CefValue> input)
{
	SYNC_ACCESS();

	QMenu menu;
	bool result = DeserializeMenu(input, menu);

	if (result) {
		m_auxMenuItems = input->Copy();
	}

	Update();

	SaveConfig();

	return result;
}

void StreamElementsMenuManager::Reset()
{
	SYNC_ACCESS();

	m_auxMenuItems->SetNull();

	Update();

	SaveConfig();
}

void StreamElementsMenuManager::SerializeAuxiliaryMenuItems(
	CefRefPtr<CefValue> &output)
{
	output = m_auxMenuItems->Copy();
}

void StreamElementsMenuManager::SaveConfig()
{
	SYNC_ACCESS();

	StreamElementsConfig::GetInstance()->SetAuxMenuItemsConfig(
		CefWriteJSON(m_auxMenuItems, JSON_WRITER_DEFAULT).ToString());
}

void StreamElementsMenuManager::LoadConfig()
{
	SYNC_ACCESS();

	CefRefPtr<CefValue> val = CefParseJSON(
		StreamElementsConfig::GetInstance()->GetAuxMenuItemsConfig(),
		JSON_PARSER_ALLOW_TRAILING_COMMAS);

	if (!val.get() || val->GetType() != VTYPE_LIST)
		return;

	DeserializeAuxiliaryMenuItems(val);
}
