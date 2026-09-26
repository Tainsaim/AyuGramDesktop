"""
quotify_plus_desktop.py
========================

Портированная версия плагина Quotify+ (оригинал — Android/exteraGram,
автор @mur_live) под новый Python Plugin API AyuGram Desktop
(см. desktop_plugin_api.py).

ЧТО ПЕРЕНЕСЕНО ПОЧТИ ДОСЛОВНО (чистый Python + PIL, без Android):
  - компоновка карточки цитаты (скруглённые углы, тень, фон)
  - разбор простого markdown (**bold**, __italic__, `mono`) для текста цитаты
  - подбор и перенос текста по ширине карточки

ЧТО СОЗНАТЕЛЬНО УПРОЩЕНО/УБРАНО в этой первой версии (см. README):
  - рендер кастомных эмодзи и Lottie-стикеров (PremiumEmojiRenderer,
    RLottieDrawable) — на десктопе для этого нужен отдельный рендер через
    Qt/lottie-библиотеку клиента, это отдельная задача
  - вытаскивание превью из видео/круглых видео (MediaMetadataRetriever)
  - сетевые прокси-обёртки поверх Java (_java_proxied_get/post) — на
    десктопе прокси уже настраивается самим клиентом
  - поиск пользователя по @username/номеру (TelegramUtils.get_user) —
    в этой версии автор цитаты берётся из ctx, который заполняет C++-хост
    на основе уже открытого в клиенте реплая (см. cpp/ayu_plugins.cpp)

Команды (совпадают с оригиналом по умолчанию, настраиваются в settings):
  .q  [автор]         — обычная цитата на реплай
  .fq <текст> | [автор] — фейковая цитата с произвольным текстом
"""

from __future__ import annotations

import os
import re
import uuid
from pathlib import Path
from typing import List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from desktop_plugin_api import BasePlugin, DesktopContext, HookResult, HookStrategy  # noqa: E402


# ---------------------------------------------------------------------------
# Вспомогательные функции рендера (перенесены из оригинального QuoteManager,
# адаптированы под чистый PIL без Android Bitmap/Canvas)
# ---------------------------------------------------------------------------

_FALLBACK_FONTS_LINUX = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]
_FALLBACK_FONTS_WINDOWS = [
    r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\Windows\Fonts\arial.ttf",
]
_FALLBACK_FONTS_MAC = [
    "/System/Library/Fonts/Supplemental/Arial.ttf",
]


def _find_system_font(bold: bool = False) -> Optional[str]:
    candidates: List[str] = []
    if os.name == "nt":
        candidates += _FALLBACK_FONTS_WINDOWS
    elif sys.platform == "darwin":
        candidates += _FALLBACK_FONTS_MAC
    else:
        candidates += _FALLBACK_FONTS_LINUX
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def _load_font(size: int, bold: bool = False, custom_path: Optional[str] = None) -> ImageFont.FreeTypeFont:
    if custom_path and os.path.exists(custom_path):
        try:
            return ImageFont.truetype(custom_path, size=size)
        except Exception:
            pass
    found = _find_system_font(bold=bold)
    if found:
        try:
            return ImageFont.truetype(found, size=size)
        except Exception:
            pass
    return ImageFont.load_default()


def _strip_simple_markdown(text: str) -> str:
    """Упрощённый разбор markdown: убирает разметку, оставляет текст.
    Полноценные bold/italic-стили внутри одной картинки требуют посегментного
    рендера — это можно добавить отдельным шагом, см. README (TODO)."""
    text = re.sub(r"```(\w+)?\n?(.*?)```", lambda m: m.group(2), text, flags=re.DOTALL)
    text = re.sub(r"`(.*?)`", r"\1", text)
    text = re.sub(r"\*\*(.*?)\*\*", r"\1", text)
    text = re.sub(r"__(.*?)__", r"\1", text)
    text = re.sub(r"~~(.*?)~~", r"\1", text)
    text = re.sub(r"\|\|(.*?)\|\|", r"\1", text)
    return text


def _wrap_text(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.FreeTypeFont, max_width: int) -> List[str]:
    lines: List[str] = []
    for paragraph in text.split("\n"):
        words = paragraph.split(" ")
        current = ""
        for word in words:
            trial = f"{current} {word}".strip()
            bbox = draw.textbbox((0, 0), trial, font=font)
            if bbox[2] - bbox[0] <= max_width or not current:
                current = trial
            else:
                lines.append(current)
                current = word
        lines.append(current)
    return lines


def _make_round_avatar(path: Optional[str], size: int, fallback_letter: str = "?") -> Image.Image:
    if path and os.path.exists(path):
        try:
            avatar = Image.open(path).convert("RGBA").resize((size, size))
        except Exception:
            avatar = None
    else:
        avatar = None

    if avatar is None:
        avatar = Image.new("RGBA", (size, size), (70, 130, 180, 255))
        draw = ImageDraw.Draw(avatar)
        font = _load_font(size // 2, bold=True)
        letter = (fallback_letter or "?")[:1].upper()
        bbox = draw.textbbox((0, 0), letter, font=font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        draw.text(((size - tw) / 2, (size - th) / 2 - bbox[1]), letter, font=font, fill=(255, 255, 255, 255))

    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size, size), fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(avatar, (0, 0), mask)
    return out


class QuoteCardRenderer:
    """Рисует карточку-цитату: аватар + имя автора + текст. Портировано из
    QuoteManager._create_base_card / generate_quote оригинального плагина."""

    def __init__(self, plugin: "QuotifyDesktopPlugin"):
        self.plugin = plugin

    def render(self, author_name: str, avatar_path: Optional[str], quote_text: str) -> str:
        quote_text = _strip_simple_markdown(quote_text).strip() or " "

        width = int(self.plugin.get_setting("card_width", 900))
        padding = 48
        avatar_size = int(self.plugin.get_setting("avatar_size", 96))
        quote_font_size = int(self.plugin.get_setting("quote_font_size", 40))
        author_font_size = int(self.plugin.get_setting("author_font_size", 32))
        bg_color = tuple(self.plugin.get_setting("bg_color", [35, 38, 43, 255]))
        text_color = tuple(self.plugin.get_setting("text_color", [235, 235, 235, 255]))
        author_color = tuple(self.plugin.get_setting("author_color", [130, 190, 255, 255]))

        custom_font_path = self.plugin.get_setting("font_path", None)
        quote_font = _load_font(quote_font_size, custom_path=custom_font_path)
        author_font = _load_font(author_font_size, bold=True, custom_path=custom_font_path)

        text_area_width = width - padding * 2 - avatar_size - 24

        probe = Image.new("RGBA", (10, 10))
        probe_draw = ImageDraw.Draw(probe)
        lines = _wrap_text(probe_draw, quote_text, quote_font, text_area_width)

        line_height = int(quote_font_size * 1.35)
        text_block_height = line_height * len(lines)
        author_block_height = int(author_font_size * 1.6)
        height = padding * 2 + max(avatar_size, text_block_height + author_block_height)

        card = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        shadow = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle(
            (10, 10, width - 10, height - 10), radius=36, fill=(0, 0, 0, 90)
        )
        card.paste(shadow.filter(ImageFilter.GaussianBlur(14)), (0, 0))
        draw = ImageDraw.Draw(card)
        draw.rounded_rectangle((0, 0, width, height), radius=36, fill=bg_color)

        avatar_img = _make_round_avatar(avatar_path, avatar_size, fallback_letter=author_name)
        avatar_y = padding
        card.paste(avatar_img, (padding, avatar_y), avatar_img)

        text_x = padding + avatar_size + 24
        author_bbox = draw.textbbox((0, 0), author_name, font=author_font)
        draw.text((text_x, padding - 4), author_name, font=author_font, fill=author_color)

        text_y = padding + (author_bbox[3] - author_bbox[1]) + 16
        for line in lines:
            draw.text((text_x, text_y), line, font=quote_font, fill=text_color)
            text_y += line_height

        out_dir = Path(self.plugin.temp_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"quotify_{uuid.uuid4().hex}.png"
        card.save(out_path, "PNG")
        return str(out_path)


class QuotifyDesktopPlugin(BasePlugin):
    id = "quotify_plus_desktop"
    name = "[Desktop Port] Quotify+"
    author = "@mur_live & @Raitorinkus (оригинал), десктоп-порт — сообщество"
    version = "0.1.0-desktop"
    description = (
        "Десктоп-порт Quotify+: делает картинку-цитату из реплая (.q) "
        "или с произвольным текстом (.fq). Часть функций Android-версии "
        "(кастомные эмодзи, стикеры, видео-превью) в этой версии не перенесена."
    )

    def __init__(self, storage_dir):
        super().__init__(storage_dir)
        self.temp_dir = storage_dir / "tmp"
        self.renderer = QuoteCardRenderer(self)

    def on_plugin_load(self) -> None:
        print(f"[{self.id}] загружен, версия {self.version}")

    def on_plugin_unload(self) -> None:
        print(f"[{self.id}] выгружен")

    def on_before_send(self, ctx: DesktopContext) -> HookResult:
        text = (ctx.message_text or "").strip()
        if not text:
            return HookResult()

        q_cmd = self.get_setting("q_cmd", ".q")
        fq_cmd = self.get_setting("fq_cmd", ".fq")
        sep = self.get_setting("fake_name_separator", "|")

        parts = text.split(" ", 1)
        cmd = parts[0].lower()
        rest = parts[1].strip() if len(parts) > 1 else ""

        if cmd == q_cmd:
            if not ctx.reply_to_text:
                ctx.show_error("⚠️ Нужно ответить (reply) на сообщение, чтобы сделать цитату.")
                return HookResult(strategy=HookStrategy.CANCEL)

            author = rest or ctx.reply_to_author_name or "Unknown"
            path = self.renderer.render(author, ctx.reply_to_author_photo_path, ctx.reply_to_text)
            return HookResult(strategy=HookStrategy.CANCEL, photo_path=path)

        if cmd == fq_cmd:
            if not rest:
                ctx.show_error("⚠️ Укажи текст после команды, например: .fq Привет мир | Автор")
                return HookResult(strategy=HookStrategy.CANCEL)

            fake_text, author = rest, "Anonymous"
            if sep and sep in rest:
                fake_text, author = (p.strip() for p in rest.rsplit(sep, 1))

            path = self.renderer.render(author, None, fake_text)
            return HookResult(strategy=HookStrategy.CANCEL, photo_path=path)

        return HookResult()


plugin_instance = QuotifyDesktopPlugin


if __name__ == "__main__":
    # Локальная самопроверка без собранного клиента: генерирует PNG
    # и показывает, что рендер реально работает.
    import tempfile

    storage = Path(tempfile.mkdtemp(prefix="quotify_test_"))
    plugin = QuotifyDesktopPlugin(storage)
    plugin.on_plugin_load()

    ctx = DesktopContext(
        message_text=".q Тестовый Автор",
        chat_id=12345,
        reply_to_text="Это пример текста реплая, на который делаем цитату. "
        "Он достаточно длинный, чтобы проверить перенос строк.",
        reply_to_author_name="Иван Иванов",
        reply_to_author_photo_path=None,
    )
    result = plugin.on_before_send(ctx)
    print("HookResult:", result)
    if result.photo_path:
        print("Картинка сохранена:", result.photo_path)
