// This file is part of AyuGram Desktop.
//
// ayu/ayu_plugins.h
//
// Менеджер Python-плагинов: встраивает CPython (через pybind11),
// сканирует папку с .py-файлами, даёт C++-коду точку вызова перед
// отправкой текстового сообщения.
//
// ВАЖНО: это стартовый скелет, а не готовый к продакшену модуль.
// Реальные типы (History*, PeerData*, HistoryItem*) нужно подставить
// из актуальных заголовков tdesktop — см. TODO-комментарии ниже и
// README в корне поставки.

#pragma once

#include <QtCore/QString>
#include <QtCore/QObject>
#include <QtCore/QFileSystemWatcher>
#include <memory>
#include <optional>

namespace Ayu::Plugins {

struct BeforeSendResult {
	bool cancelOriginalSend = false;
	QString photoPathToSend; // если непусто и cancelOriginalSend == true —
	                          // нужно отправить этот файл как фото вместо текста
	QString errorToShowUser; // если непусто — показать тост/ошибку пользователю
};

// Данные, которые C++ передаёт в Python-хук. Заполняются в history_widget.cpp
// непосредственно перед вызовом dispatchBeforeSend (см. PATCH в README).
struct BeforeSendContext {
	QString messageText;
	qint64 chatId = 0;

	bool hasReply = false;
	QString replyText;
	QString replyAuthorName;
	qint64 replyAuthorId = 0;
	QString replyAuthorPhotoPath; // путь к уже скачанному в кэш файлу аватара, если есть
};

class Manager : public QObject {
	Q_OBJECT

public:
	static Manager &instance();

	// Вызывается один раз при старте клиента (например, из Core::Application
	// или из точки, где AyuGram уже инициализирует свои сервисы — см. ayu_infra.cpp).
	void initialize(const QString &pluginsDir, const QString &storageDir);

	void shutdown();

	// Главная точка входа: вызвать перед фактической отправкой сообщения.
	// Возвращает cancelOriginalSend == true, если один из плагинов подменил
	// отправку (например, отправляет фото вместо текста).
	[[nodiscard]] BeforeSendResult dispatchBeforeSend(const BeforeSendContext &ctx);

	// Список id известных плагинов + их состояние вкл/выкл — для страницы настроек.
	struct PluginInfo {
		QString id;
		QString name;
		QString version;
		QString description;
		bool enabled = false;
	};
	[[nodiscard]] QVector<PluginInfo> listPlugins() const;
	void setPluginEnabled(const QString &pluginId, bool enabled);

	// Принудительно пересканировать папку (например, по кнопке в настройках,
	// в дополнение к автоматическому QFileSystemWatcher).
	void rescan();

private:
	Manager();
	~Manager() override;

	struct Private;
	std::unique_ptr<Private> _d;

	QFileSystemWatcher _watcher;
};

} // namespace Ayu::Plugins
