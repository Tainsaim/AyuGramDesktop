// This file is part of AyuGram Desktop.
//
// ayu/ayu_plugins.cpp
//
// См. ayu_plugins.h для описания контракта. Здесь — встраивание CPython
// через pybind11::embed и мост к desktop_plugin_api.PluginManager.
//
// ЗАВИСИМОСТИ (см. README/CMakeLists_snippet.txt):
//   find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
//   pybind11 (как external-модуль, аналогично другим external_* в проекте)
//
// СТАТУС: рабочий скелет. Компилировать и line-by-line сверять с реальными
// версиями pybind11/tdesktop нужно на месте — см. README, пункт
// "Что нужно доделать руками".

#include "ayu/ayu_plugins.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QDebug>

namespace py = pybind11;

namespace Ayu::Plugins {

namespace {

// Прокси-объект, который прокидывается в Python как `ctx._host`.
// Методы вызываются из Python (send_photo/show_toast) и должны довести
// действие до реального UI-потока Qt (через invokeMethod/crl::on_main,
// т.к. Python может дёргаться не из UI-потока — зависит от того, как
// вы организуете вызов dispatchBeforeSend).
class HostBridge {
public:
	void send_photo(qint64 chatId, const std::string &path, const std::string &caption) {
		// TODO: реальная отправка фото. В tdesktop это, в общих чертах,
		// формирование Api::SendAction + Api::MessageToSend с изображением
		// и вызов session().api().sendFile(...)/аналогичного метода —
		// нужно свериться с актуальным apiwrap.h на момент интеграции.
		qDebug() << "[ayu_plugins] send_photo requested:"
		         << chatId << QString::fromStdString(path);
		_pendingPhotoPath = QString::fromStdString(path);
		_pendingCaption = QString::fromStdString(caption);
	}

	void show_toast(const std::string &text) {
		qDebug() << "[ayu_plugins] plugin error/toast:" << QString::fromStdString(text);
		_pendingError = QString::fromStdString(text);
	}

	QString takePendingPhotoPath() { return std::exchange(_pendingPhotoPath, QString()); }
	QString takePendingCaption() { return std::exchange(_pendingCaption, QString()); }
	QString takePendingError() { return std::exchange(_pendingError, QString()); }

private:
	QString _pendingPhotoPath;
	QString _pendingCaption;
	QString _pendingError;
};

} // namespace

struct Manager::Private {
	std::optional<py::scoped_interpreter> interpreter;
	py::object pluginManagerModule;
	py::object pluginManagerInstance;
	std::shared_ptr<HostBridge> host;
	QString pluginsDir;
	QString storageDir;
	bool ready = false;
};

Manager &Manager::instance() {
	static Manager instance;
	return instance;
}

Manager::Manager() : _d(std::make_unique<Private>()) {
}

Manager::~Manager() = default;

void Manager::initialize(const QString &pluginsDir, const QString &storageDir) {
	QDir().mkpath(pluginsDir);
	QDir().mkpath(storageDir);
	_d->pluginsDir = pluginsDir;
	_d->storageDir = storageDir;

	try {
		_d->interpreter.emplace();

		// Прокидываем путь до desktop_plugin_api.py и папки plugins/,
		// чтобы Python мог их импортировать.
		py::module_ sysModule = py::module_::import("sys");
		sysModule.attr("path").attr("insert")(0, pluginsDir.toStdString());
		// Путь к каталогу с desktop_plugin_api.py — обычно рядом с exe
		// или в Resources; подставьте актуальный QStandardPaths-путь.
		const auto apiDir = QCoreApplication::applicationDirPath() + "/plugins_runtime";
		sysModule.attr("path").attr("insert")(0, apiDir.toStdString());

		py::module_ apiModule = py::module_::import("desktop_plugin_api");
		_d->pluginManagerModule = apiModule;

		auto managerClass = apiModule.attr("PluginManager");
		_d->pluginManagerInstance = managerClass(
			pluginsDir.toStdString(),
			storageDir.toStdString(),
			py::none());

		_d->pluginManagerInstance.attr("scan_and_load")();
		_d->ready = true;

		qDebug() << "[ayu_plugins] Python plugin manager initialized," << pluginsDir;
	} catch (const py::error_already_set &e) {
		qWarning() << "[ayu_plugins] Python init failed:" << e.what();
		_d->ready = false;
	}

	// Hot-reload: следим за .py файлами в папке плагинов.
	_watcher.addPath(pluginsDir);
	QObject::connect(&_watcher, &QFileSystemWatcher::directoryChanged, [this] {
		rescan();
	});
}

void Manager::shutdown() {
	if (_d->ready) {
		try {
			_d->pluginManagerInstance = py::object();
			_d->pluginManagerModule = py::object();
		} catch (...) {
		}
	}
	_d->interpreter.reset();
	_d->ready = false;
}

void Manager::rescan() {
	if (!_d->ready) return;
	try {
		_d->pluginManagerInstance.attr("check_for_changes")();
	} catch (const py::error_already_set &e) {
		qWarning() << "[ayu_plugins] rescan failed:" << e.what();
	}
}

BeforeSendResult Manager::dispatchBeforeSend(const BeforeSendContext &ctx) {
	BeforeSendResult out;
	if (!_d->ready) return out;

	try {
		auto apiModule = _d->pluginManagerModule;
		auto ctxClass = apiModule.attr("DesktopContext");

		auto pyCtx = ctxClass(
			py::arg("message_text") = ctx.messageText.toStdString(),
			py::arg("chat_id") = ctx.chatId,
			py::arg("reply_to_text") = ctx.hasReply
				? py::cast(ctx.replyText.toStdString())
				: py::none(),
			py::arg("reply_to_author_name") = ctx.hasReply
				? py::cast(ctx.replyAuthorName.toStdString())
				: py::none(),
			py::arg("reply_to_author_id") = ctx.hasReply
				? py::cast(ctx.replyAuthorId)
				: py::none(),
			py::arg("reply_to_author_photo_path") = ctx.replyAuthorPhotoPath.isEmpty()
				? py::none()
				: py::cast(ctx.replyAuthorPhotoPath.toStdString()));

		auto result = _d->pluginManagerInstance.attr("dispatch_before_send")(pyCtx);

		const auto strategyName = py::str(result.attr("strategy").attr("name")).cast<std::string>();
		if (strategyName == "CANCEL") {
			out.cancelOriginalSend = true;
			auto photoPath = result.attr("photo_path");
			if (!photoPath.is_none()) {
				out.photoPathToSend = QString::fromStdString(photoPath.cast<std::string>());
			}
			auto errorMessage = result.attr("error_message");
			if (!errorMessage.is_none()) {
				out.errorToShowUser = QString::fromStdString(errorMessage.cast<std::string>());
			}
		}
	} catch (const py::error_already_set &e) {
		qWarning() << "[ayu_plugins] dispatchBeforeSend failed:" << e.what();
	}

	return out;
}

QVector<Manager::PluginInfo> Manager::listPlugins() const {
	QVector<PluginInfo> out;
	if (!_d->ready) return out;
	try {
		auto plugins = _d->pluginManagerInstance.attr("plugins");
		for (auto item : plugins.cast<py::dict>()) {
			auto plugin = item.second;
			PluginInfo info;
			info.id = QString::fromStdString(py::str(plugin.attr("id")).cast<std::string>());
			info.name = QString::fromStdString(py::str(plugin.attr("name")).cast<std::string>());
			info.version = QString::fromStdString(py::str(plugin.attr("version")).cast<std::string>());
			info.description = QString::fromStdString(py::str(plugin.attr("description")).cast<std::string>());
			info.enabled = _d->pluginManagerInstance.attr("is_enabled")(info.id.toStdString()).cast<bool>();
			out.push_back(info);
		}
	} catch (const py::error_already_set &e) {
		qWarning() << "[ayu_plugins] listPlugins failed:" << e.what();
	}
	return out;
}

void Manager::setPluginEnabled(const QString &pluginId, bool enabled) {
	if (!_d->ready) return;
	try {
		_d->pluginManagerInstance.attr("set_enabled")(pluginId.toStdString(), enabled);
	} catch (const py::error_already_set &e) {
		qWarning() << "[ayu_plugins] setPluginEnabled failed:" << e.what();
	}
}

} // namespace Ayu::Plugins
