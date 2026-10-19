#include "dock.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <browser-panel.hpp>

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
QPointer<QWidget> container;
QPointer<QLabel> label;
QCefWidget *browser = nullptr;
QCef *cef = nullptr;
} // namespace

void dock::create()
{
	container = new QWidget();
	container->setObjectName("multistreamDock");
	container->setMinimumSize(300, 420);
	container->setStyleSheet("#multistreamDock { background: #0e0e13; } QLabel { color: #8a89a0; }");
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	label = new QLabel("Loading Multistream...");
	label->setAlignment(Qt::AlignCenter);
	label->setWordWrap(true);
	layout->addWidget(label);
	if (!obs_frontend_add_dock_by_id("multistream", "Multistream", container))
		obs_log(LOG_WARNING, "Multistream dock: OBS already has a dock called multistream");
}

void dock::loadPage(const QString &url)
{
	if (!container || browser)
		return;
	cef = obs_browser_init_panel();
	if (!cef) {
		if (label)
			label->setText("The Multistream dock needs OBS's browser, which this OBS doesn't have.");
		return;
	}
	cef->init_browser();
	cef->wait_for_browser_init();
	browser = cef->create_widget(container, url.toStdString(), nullptr);
	if (!browser) {
		if (label)
			label->setText("The Multistream dock couldn't open its page.");
		return;
	}
	if (label) {
		label->hide();
		label->deleteLater();
	}
	container->layout()->addWidget(browser);
}

// The first time the plugin runs, OBS would keep its new dock hidden in
// the Docks menu: show it, so a new streamer finds it.
void dock::showOnFirstRun()
{
	char *file = obs_module_config_path("dock.json");
	obs_data_t *d = obs_data_create_from_json_file_safe(file, "bak");
	if (!d)
		d = obs_data_create();
	if (!obs_data_get_bool(d, "shown") && container) {
		if (auto *w = qobject_cast<QDockWidget *>(container->parentWidget())) {
			// OBS adds plugin docks floating: put it on the right of
			// OBS's window, where it can't get lost behind it.
			auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (main) {
				w->setFloating(false);
				main->addDockWidget(Qt::RightDockWidgetArea, w);
				main->resizeDocks({w}, {360}, Qt::Horizontal);
			}
			w->setVisible(true);
			w->raise();
		}
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		obs_data_set_bool(d, "shown", true);
		obs_data_save_json_safe(d, file, "tmp", "bak");
	}
	obs_data_release(d);
	bfree(file);
}

void dock::close()
{
	if (browser) {
		browser->closeBrowser();
		browser = nullptr;
	}
}
