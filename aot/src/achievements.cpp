// achievements.cpp - The game's achievements in its language, by section.
//
// The game unlocks its achievements through XGIUserWriteAchievements; the
// runtime (AchievementManager) keeps them in <user_data_root>\achievements\
// 4541084C.toml and shows a notice. Its catalog comes from the title's XDBF
// when the XEX loads, before lang::Install sets the console's language
// (user_language), so it is always English. Setup builds it again in the
// game's language: the disc has English, German, French, Spanish and Italian;
// Russian is the table below. Unlocks are kept by id, untouched.
//
// The launcher shows them too, but the text and the icons are the game's, not
// the port's: Setup writes them, every language, to <cache_root>\achievements
// (catalog.tsv, icons\<image id>.png), which every launcher profile shares.
// The unlocks are each profile's own.
//
// The sections group the 41 achievements for the Achievements page (Options >
// Graphics > Achievements, graphics_menu.cpp), at most seven to a section: the
// page's rows.
#include "achievements.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

#include "game_language.h"

namespace
{
  using rex::system::XLanguage;

  struct Section
  {
    const char *name;
    uint32_t ids[ao2::ach::kSectionSize]; // 0: no more
  };

  constexpr Section kSections[ao2::ach::kSectionCount] = {
      {"Story", {7, 8, 9, 10, 11, 12, 66}},
      {"Campaign", {15, 17, 22, 24, 40, 54}},
      {"Gear and co-op", {20, 21, 64, 26, 27, 59, 65}},
      {"Weapons", {30, 31, 32, 33, 34, 35, 36}},
      {"Combat", {37, 38, 39, 60, 61}},
      {"Versus", {48, 49, 50, 51, 55}},
      {"SSC Challenge", {67, 68, 69, 70}},
  };

  int SectionOf(uint32_t id)
  {
    for (int s = 0; s < ao2::ach::kSectionCount; ++s)
    {
      for (uint32_t i : kSections[s].ids)
      {
        if (i == id)
        {
          return s;
        }
      }
    }
    return -1;
  }

  // The Russian text (the disc has none). The words are the Russian
  // localization's: Контрактник / Профессионал, агро, ОВЕРКИЛЛ, парный
  // снайпинг, спина к спине, Противостояние, рейтинговый матч.
  struct Russian
  {
    uint32_t id;
    const char *label, *description, *locked; // locked: nullptr when it is the description
  };

  constexpr const char *kSecretRu = "Продолжайте играть, чтобы открыть это секретное достижение.";

  constexpr Russian kRussian[] = {
      {7, "Миссия 1 пройдена", "Пройдите миссию 1 на уровне «Контрактник» или выше.", kSecretRu},
      {8, "Миссия 2 пройдена", "Пройдите миссию 2 на уровне «Контрактник» или выше.", kSecretRu},
      {9, "Миссия 3 пройдена", "Пройдите миссию 3 на уровне «Контрактник» или выше.", kSecretRu},
      {10, "Миссия 4 пройдена", "Пройдите миссию 4 на уровне «Контрактник» или выше.", kSecretRu},
      {11, "Миссия 5 пройдена", "Пройдите миссию 5 на уровне «Контрактник» или выше.", kSecretRu},
      {12, "Миссия 6 пройдена", "Пройдите миссию 6 на уровне «Контрактник» или выше.", kSecretRu},
      {15, "Элитная ЧВК", "Пройдите все миссии на уровне «Профессионал».", nullptr},
      {17, "Мой виртуальный друг", "Пройдите все миссии с напарником под управлением компьютера.", nullptr},
      {20, "Поздоровайся с моими друзьями", "Купите MP7, M134, DAO-X Protecta, MGL MK-X и M107 Light Fifty.", nullptr},
      {21, "Человек многих масок", "Купите все варианты масок.", nullptr},
      {22, "Если б я был богат", "Заработайте в сумме миллион долларов в кампании.", nullptr},
      {24, "Страх убивает разум", "Продержитесь на полном агро 1 минуту без перерыва.", nullptr},
      {26, "Поднять бунт", "Убейте в сумме 50 врагов в кооперативном приеме со щитом.", nullptr},
      {27, "Две пары глаз лучше одной", "Убейте в сумме 5 врагов парным снайпингом.", nullptr},
      {30, "Семь шестьдесят два", "Убейте в сумме 250 врагов из штурмовых винтовок.", nullptr},
      {31, "Это мой бумстик!", "Убейте в сумме 250 врагов из дробовиков.", nullptr},
      {32, "Стреляй и молись", "Убейте в сумме 250 врагов из пулеметов.", nullptr},
      {33, "Рука мертвеца", "Убейте в сумме 150 врагов из пистолетов.", nullptr},
      {34, "Ну, кто еще хочет?", "Убейте в сумме 250 врагов из пистолетов-пулеметов.", nullptr},
      {35, "Один выстрел - один труп", "Убейте в сумме 100 врагов из снайперских винтовок.", nullptr},
      {36, "Большой бада-бум!", "Убейте в сумме 25 врагов из РПГ.", nullptr},
      {37, "Получи. Получи по-настоящему", "Убейте в сумме 50 врагов в ближнем бою.", nullptr},
      {38, "Fission Mailed", "Убейте 25 смертников выстрелом в бомбу на груди.", nullptr},
      {39, "Если кровоточит - убить можно", "Убейте 25 тяжелых солдат.", nullptr},
      {40, "Полевой медик", "Оттащите и вылечите напарника 25 раз.", nullptr},
      {48, "Сапоги на земле", "Доиграйте рейтинговый матч в Противостоянии.", nullptr},
      {49, "Дьявол в мелочах", "Выполните 20 второстепенных задач в рейтинговом матче.", nullptr},
      {50, "Выжить в игре", "Завершите рейтинговый матч, ни разу не нуждаясь в лечении.", nullptr},
      {51, "Пенсионные накопления", "Заработайте в сумме миллиард долларов в рейтинговых матчах.", nullptr},
      {54, "Долг платежом красен", "Впервые вылечите напарника. Обучение не в счет.", nullptr},
      {55, "Одного ствола хватит", "Победите в рейтинговом матче, не покупая другого оружия.", nullptr},
      {59, "Зверь о двух фронтах", "Убейте в сумме 50 врагов в приеме «Спина к спине».", nullptr},
      {60, "Бегущий человек", "Убейте 75 врагов в режиме «Оверкилл: скрытность».", nullptr},
      {61, "Бойня", "Убейте 100 врагов в силовом режиме «Оверкилл».", nullptr},
      {64, "Знаток оружия", "Купите 3 единицы оружия.", nullptr},
      {65, "Каменная стена", "Убейте 30 врагов ударом щита.", nullptr},
      {66, "Мой кейс!", "Соберите все кейсы с информацией в игре.", nullptr},
      {67, "Настоящий претендент SSC", "Выиграйте все этапы рейтингового матча SSC Challenge из трех раундов.",
       nullptr},
      {68, "Бочки SSC", "Уничтожьте 500 бочек в рейтинговых матчах SSC Challenge.", nullptr},
      {69, "Победа в SSC Challenge", "Выиграйте рейтинговый матч SSC Challenge.", nullptr},
      {70, "Снайпер SSC", "Завершите рейтинговый матч SSC Challenge из трех раундов с точностью 75%.", nullptr},
  };

  const Russian *RussianOf(uint32_t id)
  {
    for (const Russian &r : kRussian)
    {
      if (r.id == id)
      {
        return &r;
      }
    }
    return nullptr;
  }

  // The launcher's codes, the disc's languages.
  constexpr std::pair<const char *, XLanguage> kDiscLanguages[] = {
      {"en", XLanguage::kEnglish}, {"de", XLanguage::kGerman},  {"fr", XLanguage::kFrench},
      {"es", XLanguage::kSpanish}, {"it", XLanguage::kItalian},
  };

  struct Text
  {
    std::string label, description, locked;
  };

  Text TextIn(const rex::system::util::XdbfGameData &db, const rex::system::util::XdbfAchievementTableEntry &e,
              const char *code)
  {
    if (std::strcmp(code, "ru") == 0)
    {
      if (const Russian *r = RussianOf(uint32_t(e.id)))
      {
        return {r->label, r->description, r->locked ? r->locked : r->description};
      }
      code = "en";
    }
    XLanguage lang = XLanguage::kEnglish;
    for (const auto &[c, l] : kDiscLanguages)
    {
      if (std::strcmp(c, code) == 0)
      {
        lang = l;
      }
    }
    lang = db.GetExistingLanguage(lang);
    return {db.GetStringTableEntry(lang, uint16_t(e.label_id)), db.GetStringTableEntry(lang, uint16_t(e.description_id)),
            db.GetStringTableEntry(lang, uint16_t(e.unachieved_id))};
  }

  // Tabs and line breaks out: catalog.tsv is a line per text.
  std::string Field(std::string s)
  {
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '\t' || c == '\r' || c == '\n'; }, ' ');
    return s;
  }

  // Writes `bytes` to `path` unless it already holds them.
  void WriteIfChanged(const std::filesystem::path &path, const std::string &bytes)
  {
    {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream old;
      old << in.rdbuf();
      if (in && old.str() == bytes)
      {
        return;
      }
    }
    std::filesystem::path part = path;
    part += ".tmp";
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!out.flush())
      {
        return;
      }
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec)
    {
      std::filesystem::remove(part, ec);
    }
  }

  // catalog.tsv and the icons for the launcher.
  void Export(const std::filesystem::path &cache_root, const rex::system::util::XdbfGameData &db)
  {
    namespace fs = std::filesystem;
    fs::path dir = cache_root / "achievements";
    std::error_code ec;
    fs::create_directories(dir / "icons", ec);
    if (ec)
    {
      REXLOG_WARN("achievements: cannot create {}: {}", rex::path_to_utf8(dir), ec.message());
      return;
    }
    // A line per achievement (A), then one per language (T):
    //   A  id  section  gamerscore  image id  secret
    //   T  id  language  label  description  description while locked
    std::string tsv = "# Army of Two achievements, written by the game for the launcher\n";
    for (const auto &e : db.GetAchievements())
    {
      bool secret = !(e.flags & 0x8); // XDBF: shown before it is unlocked
      tsv += "A\t" + std::to_string(e.id) + "\t" + std::to_string(SectionOf(e.id)) + "\t" +
             std::to_string(e.gamerscore) + "\t" + std::to_string(e.image_id) + "\t" + (secret ? "1" : "0") + "\n";
      for (const char *code : {"en", "ru", "de", "fr", "es", "it"})
      {
        Text t = TextIn(db, e, code);
        tsv += "T\t" + std::to_string(e.id) + "\t" + code + "\t" + Field(t.label) + "\t" + Field(t.description) +
               "\t" + Field(t.locked) + "\n";
      }
      auto icon = db.GetEntry(rex::system::util::XdbfSection::kImage, e.image_id);
      fs::path png = dir / "icons" / (std::to_string(e.image_id) + ".png");
      if (icon && !fs::exists(png, ec))
      {
        WriteIfChanged(png, std::string(reinterpret_cast<const char *>(icon.buffer), icon.size));
      }
    }
    WriteIfChanged(dir / "catalog.tsv", tsv);
  }
} // namespace

namespace ao2::ach
{
  void Setup(const std::filesystem::path &cache_root)
  {
    auto *ks = rex::system::kernel_state();
    if (!ks)
    {
      return;
    }
    const auto db = ks->title_xdbf();
    if (!db.is_valid())
    {
      REXLOG_WARN("achievements: no XDBF in the title");
      return;
    }
    const char *code = ao2::lang::Code();
    std::vector<rex::system::AchievementInfo> list;
    for (const auto &e : db.GetAchievements())
    {
      Text t = TextIn(db, e, code);
      rex::system::AchievementInfo info;
      info.id = uint32_t(e.id);
      info.label = t.label;
      info.description = t.description;
      info.unachieved_description = t.locked;
      info.image_id = uint32_t(e.image_id);
      info.gamerscore = uint32_t(e.gamerscore);
      info.flags = uint32_t(e.flags);
      list.push_back(std::move(info));
    }
    size_t count = list.size();
    ks->achievements().ReplaceAchievements(std::move(list));
    REXLOG_INFO("achievements: {} in '{}', {} unlocked", count, code, Count().unlocked);
    if (!cache_root.empty())
    {
      Export(cache_root, db);
    }
  }

  const char *SectionName(int section)
  {
    return section >= 0 && section < kSectionCount ? kSections[section].name : "";
  }

  std::vector<Item> Section(int section)
  {
    std::vector<Item> items;
    auto *ks = rex::system::kernel_state();
    if (!ks || section < 0 || section >= kSectionCount)
    {
      return items;
    }
    const auto &manager = ks->achievements();
    for (uint32_t id : kSections[section].ids)
    {
      if (!id)
      {
        break;
      }
      auto info = manager.FindAchievement(id);
      if (!info)
      {
        continue;
      }
      Item item;
      item.id = id;
      item.label = info->label;
      item.unlocked = manager.IsUnlocked(id);
      item.description = item.unlocked ? info->description : info->unachieved_description;
      item.gamerscore = info->gamerscore;
      items.push_back(std::move(item));
    }
    return items;
  }

  Totals Count(int section)
  {
    Totals t;
    auto *ks = rex::system::kernel_state();
    if (!ks)
    {
      return t;
    }
    const auto &manager = ks->achievements();
    for (const auto &info : manager.ListAchievements())
    {
      if (section >= 0 && SectionOf(info.id) != section)
      {
        continue;
      }
      bool unlocked = manager.IsUnlocked(info.id);
      ++t.count;
      t.max_gamerscore += static_cast<int>(info.gamerscore);
      if (unlocked)
      {
        ++t.unlocked;
        t.gamerscore += static_cast<int>(info.gamerscore);
      }
    }
    return t;
  }
} // namespace ao2::ach
