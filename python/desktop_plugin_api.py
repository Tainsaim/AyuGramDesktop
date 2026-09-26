"""
desktop_plugin_api.py
======================

Фасад API для Python-плагинов AyuGram Desktop.

Задача этого модуля — дать плагинам (например, портированному Quotify+)
знакомый интерфейс, максимально похожий по духу на Android-версию
(base_plugin / client_utils / hook_utils), но реализованный поверх
C++/Qt-хоста через pybind11, а не поверх JVM/Chaquopy.

Важно: этот файл сам по себе ничего не патчит и не хукает Java-методы —
на десктопе это не нужно и невозможно. Вместо этого C++-хост
(см. cpp/ayu_plugins.h/.cpp) вызывает Python-функции плагина напрямую,
в нужный момент (например, перед отправкой сообщения).

Модуль НЕ содержит рабочей логики самого клиента — только контракт
между C++ и Python. Реальные данные (текст реплая, автор и т.д.)
прокидывает C++ через объект `ctx` (см. класс DesktopContext ниже),
который в реальной сборке будет являться pybind11-объектом,
а здесь — чистым Python-классом для локальной разработки/тестов
без собранного клиента.
"""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import traceback
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path
from typing import Any, Callable, Dict, Optional


# ---------------------------------------------------------------------------
# Базовые типы (аналоги HookResult / HookStrategy из Android-версии)
# ---------------------------------------------------------------------------

class HookStrategy(Enum):
    CONTINUE = auto()   # ничего не менять, отправить сообщение как обычно
    CANCEL = auto()      # отменить исходную отправку (плагин сам решил, что делать)


@dataclass
class HookResult:
    strategy: HookStrategy = HookStrategy.CONTINUE
    # если плагин хочет отправить готовое изображение вместо текста —
    # он кладёт сюда абсолютный путь к файлу, а strategy = CANCEL
    photo_path: Optional[str] = None
    error_message: Optional[str] = None


# ---------------------------------------------------------------------------
# Контекст, который C++ передаёт в хук перед отправкой сообщения.
# В реальной сборке этот объект создаётся на стороне C++ (pybind11::class_)
# и его поля читаются напрямую из HistoryWidget/Api::SendAction.
# Здесь — Python-заглушка с тем же интерфейсом, чтобы плагин можно было
# писать и тестировать локально, без собранного клиента.
# ---------------------------------------------------------------------------

@dataclass
class DesktopContext:
    message_text: str
    chat_id: int = 0
    reply_to_text: Optional[str] = None
    reply_to_author_name: Optional[str] = None
    reply_to_author_id: Optional[int] = None
    reply_to_author_photo_path: Optional[str] = None  # путь к уже скачанному файлу аватара, если есть

    # --- методы, которые в реальной сборке дёргают C++ на стороне хоста ---
    def send_photo(self, path: str, caption: str = "") -> None:
        """Отправить готовое изображение вместо текста."""
        if self._host is not None:
            self._host.send_photo(self.chat_id, path, caption)
        else:
            print(f"[stub] send_photo(chat_id={self.chat_id}, path={path!r})")

    def show_error(self, text: str) -> None:
        if self._host is not None:
            self._host.show_toast(text)
        else:
            print(f"[stub] show_error: {text}")

    _host: Any = field(default=None, repr=False, compare=False)


# ---------------------------------------------------------------------------
# Базовый класс плагина
# ---------------------------------------------------------------------------

class BasePlugin:
    """
    Базовый класс, от которого наследуются плагины.
    Имена методов намеренно похожи на Android base_plugin, чтобы порт
    плагинов был максимально механическим.
    """

    id: str = "unknown_plugin"
    name: str = "Unknown Plugin"
    author: str = ""
    version: str = "0.0.0"
    description: str = ""

    def __init__(self, storage_dir: Path):
        self._storage_dir = storage_dir
        self._settings_path = storage_dir / f"{self.id}.settings.json"
        self._settings: Dict[str, Any] = {}
        self._load_settings()

    # --- настройки (заменяют SharedPreferences из Android) -----------------
    def _load_settings(self) -> None:
        if self._settings_path.exists():
            try:
                self._settings = json.loads(self._settings_path.read_text("utf-8"))
            except Exception:
                self._settings = {}

    def _save_settings(self) -> None:
        self._settings_path.parent.mkdir(parents=True, exist_ok=True)
        self._settings_path.write_text(
            json.dumps(self._settings, ensure_ascii=False, indent=2), encoding="utf-8"
        )

    def get_setting(self, key: str, default: Any = None) -> Any:
        return self._settings.get(key, default)

    def set_setting(self, key: str, value: Any) -> None:
        self._settings[key] = value
        self._save_settings()

    # --- жизненный цикл -----------------------------------------------------
    def on_plugin_load(self) -> None:
        """Вызывается один раз при включении/загрузке плагина."""

    def on_plugin_unload(self) -> None:
        """Вызывается при выключении плагина или перед hot-reload."""

    # --- хук перед отправкой сообщения --------------------------------------
    def on_before_send(self, ctx: DesktopContext) -> HookResult:
        """
        Вызывается перед отправкой текстового сообщения.
        Вернуть HookResult(strategy=CANCEL, photo_path=...) чтобы
        подменить отправку на картинку, либо HookResult() (CONTINUE)
        чтобы ничего не менять.
        """
        return HookResult()


# ---------------------------------------------------------------------------
# Загрузчик плагинов: сканирует папку, импортирует .py файлы,
# инстанцирует найденные подклассы BasePlugin, поддерживает hot-reload.
# ---------------------------------------------------------------------------

class PluginManager:
    def __init__(self, plugins_dir: str, storage_dir: str, host: Any = None):
        self.plugins_dir = Path(plugins_dir)
        self.storage_dir = Path(storage_dir)
        self.host = host  # pybind11-объект в реальной сборке, None в тестах
        self.plugins: Dict[str, BasePlugin] = {}
        self._mtimes: Dict[str, float] = {}
        self._config_path = self.storage_dir / "plugins_enabled.json"
        self._enabled: Dict[str, bool] = {}
        self._load_enabled_config()

    # --- вкл/выкл -------------------------------------------------------------
    def _load_enabled_config(self) -> None:
        if self._config_path.exists():
            try:
                self._enabled = json.loads(self._config_path.read_text("utf-8"))
            except Exception:
                self._enabled = {}

    def _save_enabled_config(self) -> None:
        self._config_path.parent.mkdir(parents=True, exist_ok=True)
        self._config_path.write_text(
            json.dumps(self._enabled, ensure_ascii=False, indent=2), encoding="utf-8"
        )

    def is_enabled(self, plugin_id: str) -> bool:
        return self._enabled.get(plugin_id, True)  # по умолчанию включены после установки

    def set_enabled(self, plugin_id: str, enabled: bool) -> None:
        self._enabled[plugin_id] = enabled
        self._save_enabled_config()
        plugin = self.plugins.get(plugin_id)
        if plugin is None:
            return
        if enabled:
            plugin.on_plugin_load()
        else:
            plugin.on_plugin_unload()

    # --- сканирование и (пере)загрузка -----------------------------------------
    def scan_and_load(self) -> None:
        if not self.plugins_dir.exists():
            return
        for py_file in sorted(self.plugins_dir.glob("*.py")):
            self._load_or_reload_file(py_file)

    def check_for_changes(self) -> None:
        """Вызывать периодически (или из QFileSystemWatcher) для hot-reload."""
        if not self.plugins_dir.exists():
            return
        for py_file in sorted(self.plugins_dir.glob("*.py")):
            mtime = py_file.stat().st_mtime
            key = str(py_file)
            if self._mtimes.get(key) != mtime:
                self._load_or_reload_file(py_file)

    def _load_or_reload_file(self, py_file: Path) -> None:
        module_name = f"ayu_plugin_{py_file.stem}"
        key = str(py_file)
        try:
            spec = importlib.util.spec_from_file_location(module_name, py_file)
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)  # type: ignore[union-attr]

            plugin_cls = None
            for attr in vars(module).values():
                if isinstance(attr, type) and issubclass(attr, BasePlugin) and attr is not BasePlugin:
                    plugin_cls = attr
                    break

            if plugin_cls is None:
                print(f"[plugins] {py_file.name}: класс-наследник BasePlugin не найден")
                return

            old = self.plugins.get(plugin_cls.id)
            if old is not None:
                old.on_plugin_unload()

            instance = plugin_cls(self.storage_dir)
            self.plugins[instance.id] = instance
            self._mtimes[key] = py_file.stat().st_mtime

            if self.is_enabled(instance.id):
                instance.on_plugin_load()

            print(f"[plugins] загружен: {instance.id} v{instance.version}")
        except Exception:
            print(f"[plugins] ошибка загрузки {py_file.name}:\n{traceback.format_exc()}")

    # --- вызов хука перед отправкой ---------------------------------------------
    def dispatch_before_send(self, ctx: DesktopContext) -> HookResult:
        for plugin_id, plugin in self.plugins.items():
            if not self.is_enabled(plugin_id):
                continue
            try:
                result = plugin.on_before_send(ctx)
            except Exception:
                print(f"[plugins] исключение в {plugin_id}.on_before_send:\n{traceback.format_exc()}")
                continue
            if result.strategy is HookStrategy.CANCEL:
                return result
        return HookResult()
