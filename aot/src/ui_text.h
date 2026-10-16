// ui_text.h - The port's own UI text in the game's language (see ui_text.cpp).
#pragma once

namespace ao2::text
{
  enum class Lang
  {
    kEn,
    kRu,
    kDe,
    kFr,
    kEs,
    kIt,
  };

  // At startup, before the UI: the language the game runs in
  // (ao2::lang::Code()).
  void Init();

  // The UI language: ao2_ui_language, "auto" = the game's.
  Lang Current();

  // The language the game's own text is in (ao2_ui_language does not change it).
  Lang Game();

  // `en` (a string literal of the table in ui_text.cpp) in the UI language;
  // `en` itself when the table has no translation. The result lives forever.
  const char *Tr(const char *en);
  // The same in `lang` (e.g. Game() for text shown in the game's own fonts).
  const char *TrIn(Lang lang, const char *en);
} // namespace ao2::text
