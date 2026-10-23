// achievements.cpp - The game's achievements in its language, and for the launcher.
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
// The sections group the 41 achievements for the launcher's tab.
//
// The unlock notice is the port's own (the SDK's toast is off: with the
// native renderer nothing draws it): a toast in the notices' overlay layer
// (debug_menu.h SetToast). A gold medallion with the icon pops in at the
// bottom of the screen, bursts, unfolds into a card with the name, a shine
// runs over it, and it folds back. Unlocks that come together queue up.
// Each toast plays a short soft tone as it shows (ao2_achievement_sound),
// synthesized here and played with PlaySound beside the game's audio.
// ao2_achievement_notices off (the launcher's switch): neither.
//
// The menu (ao2_achievements_key, F7): every achievement by section with its
// icon, score and unlock date, in a window of the menu's drawer (debug_menu.h
// OpenWindow); the texts are the game's, in its language.
#include "achievements.h"

#include <algorithm>
#include <atomic>
#include <iterator>
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

#include "debug_menu.h"
#include "game_language.h"
#include "ui_text.h"
#include "ui_theme.h"

#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

REXCVAR_DEFINE_BOOL(ao2_achievement_notices, true, "General",
                    "Show the achievement unlock notice (and its sound); off: achievements still unlock, silently");
REXCVAR_DEFINE_STRING(ao2_achievements_key, "F7", "General",
                      "Key that opens the achievements menu; empty = none");
REXCVAR_DEFINE_DOUBLE(ao2_achievement_sound, 0.4, "General",
                      "Volume of the achievement unlock sound, 0..1; 0 = no sound");

namespace
{
  using rex::system::XLanguage;

  struct Section
  {
    const char *name;
    uint32_t ids[7]; // 0: no more
  };

  constexpr Section kSections[] = {
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
    for (int s = 0; s < static_cast<int>(std::size(kSections)); ++s)
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

  // --- The unlock toast ---

  constexpr float kLife = 6.2f; // seconds on screen
  constexpr ImU32 kGold = IM_COL32(236, 192, 96, 255);
  constexpr ImU32 kGoldLight = IM_COL32(255, 232, 170, 255);

  struct Toast
  {
    rex::system::AchievementInfo info;
    double start = -1.0; // ImGui time it showed; < 0: waiting
  };

  std::mutex g_toast_mutex;
  std::deque<Toast> g_toasts; // under g_toast_mutex
  std::unique_ptr<rex::ui::AchievementIconCache> g_icons; // UI thread
  rex::ui::ImmediateDrawer *g_icons_drawer = nullptr;

  rex::ui::ImmediateTexture *IconOf(const rex::system::AchievementInfo &info)
  {
    rex::ui::ImmediateDrawer *drawer = ao2::menu::NoticeImmediateDrawer();
    if (!drawer)
    {
      return nullptr;
    }
    if (!g_icons || g_icons_drawer != drawer)
    {
      g_icons = std::make_unique<rex::ui::AchievementIconCache>(drawer, rex::Runtime::instance());
      g_icons_drawer = drawer;
    }
    return g_icons->GetIcon(info);
  }

  // 0..1 over [from, from + len].
  float Seg(float t, float from, float len)
  {
    return ao2::ui::Clamp01((t - from) / len);
  }

  float EaseInCubic(float t)
  {
    t = ao2::ui::Clamp01(t);
    return t * t * t;
  }

  // A band of light across [x0, x1] (bright in the middle, clear at the
  // edges), its top shifted by `slant`.
  void Shine(ImDrawList *dl, float x0, float x1, float top, float bottom, float slant, ImU32 col)
  {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    ImU32 clear = col & ~IM_COL32_A_MASK;
    const float xs[3] = {x0, (x0 + x1) * 0.5f, x1};
    const ImU32 cs[3] = {clear, col, clear};
    dl->PrimReserve(12, 6);
    auto base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int i = 0; i < 3; ++i)
    {
      dl->PrimWriteVtx(ImVec2(xs[i] + slant, top), uv, cs[i]);
      dl->PrimWriteVtx(ImVec2(xs[i], bottom), uv, cs[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
      auto v = static_cast<ImDrawIdx>(base + i * 2);
      dl->PrimWriteIdx(v);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(v + 2));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(v + 3));
      dl->PrimWriteIdx(v);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(v + 3));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(v + 1));
    }
  }

  // A four-pointed sparkle.
  void Sparkle(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
  {
    float w = r * 0.28f;
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + w, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - w, c.y), col);
    dl->AddQuadFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - w), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + w), col);
  }

  // --- The unlock sound ---
  //
  // Short and soft, in the spirit of a console's unlock notice: a quick
  // rising pop, then a round tone (E6 over its lower octave) that dies away
  // in half a second, a little wide, with a small room. A 16-bit stereo WAV
  // in memory.

  constexpr int kRate = 44100;
  constexpr double kPi = 3.14159265358979, kTau = 2.0 * kPi;

  // A small room (Schroeder): four damped feedback combs, two allpasses;
  // `stretch` lengthens the delays (the right channel's, for width).
  std::vector<double> Room(const std::vector<double> &in, double stretch)
  {
    const int n = int(in.size());
    std::vector<double> wet(n, 0.0);
    for (double ms : {29.7, 37.1, 41.1, 43.7})
    {
      int d = int(ms * stretch * kRate / 1000.0);
      std::vector<double> buf(d, 0.0);
      double damp = 0.0;
      for (int i = 0; i < n; ++i)
      {
        double y = buf[i % d];
        damp = 0.6 * y + 0.4 * damp;
        buf[i % d] = in[i] + 0.7 * damp;
        wet[i] += 0.25 * y;
      }
    }
    for (double ms : {5.0, 1.7})
    {
      int d = int(ms * stretch * kRate / 1000.0);
      std::vector<double> buf(d, 0.0);
      for (int i = 0; i < n; ++i)
      {
        double b = buf[i % d];
        double x = wet[i] + 0.5 * b;
        buf[i % d] = x;
        wet[i] = b - 0.5 * x;
      }
    }
    return wet;
  }

  std::vector<uint8_t> MakeUnlockSound(double volume)
  {
    constexpr double kSeconds = 1.0;
    const int n = int(kRate * kSeconds);
    std::vector<double> left(n, 0.0), right(n, 0.0);
    double pop_phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
      double t = double(i) / kRate;
      // The pop: 40 ms, swept up from 380 to 1320 Hz.
      double sweep = 1.0 - std::exp(-t / 0.012);
      pop_phase += kTau * (380.0 + 940.0 * sweep) / kRate;
      double pop = t < 0.05 ? std::sin(pop_phase) * std::sin(kPi * std::min(1.0, t / 0.04)) * 0.45 : 0.0;
      // The tone from 25 ms: a smooth 10 ms attack, then it dies away.
      double tt = t - 0.025;
      double tone_l = 0.0, tone_r = 0.0;
      if (tt > 0.0)
      {
        double attack = std::sin(0.5 * kPi * std::min(1.0, tt / 0.010));
        double env = attack * attack * std::exp(-tt * 6.5);
        // E6 with a little of its octave below; each side a hair detuned.
        for (double cents : {-4.0, 4.0})
        {
          double k = std::pow(2.0, cents / 1200.0);
          double v = std::sin(kTau * 1318.51 * k * tt) + 0.35 * std::sin(kTau * 659.26 * k * tt) +
                     0.08 * std::exp(-tt * 10.0) * std::sin(kTau * 2637.02 * k * tt);
          (cents < 0.0 ? tone_l : tone_r) = env * v;
        }
      }
      left[i] = pop + 0.5 * tone_l;
      right[i] = pop + 0.5 * tone_r;
    }
    std::vector<double> wl = Room(left, 1.0), wr = Room(right, 1.07);
    for (int i = 0; i < n; ++i)
    {
      left[i] += 0.18 * wl[i];
      right[i] += 0.18 * wr[i];
    }

    double peak = 1e-9;
    for (int i = 0; i < n; ++i)
    {
      peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
    }
    double gain = 0.85 / peak * std::clamp(volume, 0.0, 1.0);
    int fade = kRate / 5; // the last 200 ms
    std::vector<uint8_t> wav(44 + size_t(n) * 4);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, uint32_t(wav.size() - 8));
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 2); // stereo
    put32(24, kRate);
    put32(28, kRate * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, uint32_t(n * 4));
    for (int i = 0; i < n; ++i)
    {
      double g = gain * std::min(1.0, double(n - i) / fade);
      put16(44 + size_t(i) * 4, uint16_t(int16_t(std::clamp(left[i] * g, -1.0, 1.0) * 32767.0)));
      put16(46 + size_t(i) * 4, uint16_t(int16_t(std::clamp(right[i] * g, -1.0, 1.0) * 32767.0)));
    }
    return wav;
  }

  // UI thread, as a toast shows. Asynchronous; the next one cuts this one.
  void PlayUnlockSound()
  {
    static std::vector<uint8_t> sound;
    static double sound_volume = -1.0;
    double volume = REXCVAR_GET(ao2_achievement_sound);
    if (volume <= 0.0)
    {
      return;
    }
    if (volume != sound_volume)
    {
      PlaySoundW(nullptr, nullptr, 0); // the old buffer is about to go
      sound = MakeUnlockSound(volume);
      sound_volume = volume;
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(sound.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
  }

  // One toast `t` seconds after it showed; false when it is over.
  bool DrawToast(const Toast &toast, float t)
  {
    namespace ui = ao2::ui;
    using ui::FontKind;
    if (t >= kLife)
    {
      return false;
    }
    const auto &info = toast.info;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float s = ui::Scale();
    float now = float(ImGui::GetTime());

    // Medallion in, card unfolds, text in ... text out, card folds, medallion out.
    float pop = ui::EaseOutBack(Seg(t, 0.0f, 0.45f));
    float unfold = ui::EaseOutCubic(Seg(t, 0.35f, 0.55f)) * (1.0f - EaseInCubic(Seg(t, kLife - 0.75f, 0.45f)));
    float text_a = Seg(t, 0.65f, 0.35f) * (1.0f - Seg(t, kLife - 0.95f, 0.25f));
    float out = EaseInCubic(Seg(t, kLife - 0.32f, 0.32f));
    float a = ui::Clamp01(t / 0.15f) * (1.0f - out);

    const char *caption = ao2::text::Tr("ACHIEVEMENT UNLOCKED");
    char score[16];
    std::snprintf(score, sizeof(score), "%u G", info.gamerscore);

    float R = 38.0f * s; // the medallion's radius
    float h = 2.0f * R + 8.0f * s;
    float text_w = std::max(ui::TextSize(FontKind::kHeading, info.label.c_str()).x,
                            ui::CapsSize(FontKind::kLabel, caption).x + 90.0f * s);
    float full_w = std::clamp(h + text_w + 60.0f * s, 420.0f * s, vp->Size.x * 0.7f);
    float w = h + (full_w - h) * unfold;
    float cx = vp->Pos.x + vp->Size.x * 0.5f;
    float bottom = vp->Pos.y + vp->Size.y * 0.88f + out * 40.0f * s;
    ImVec2 mn(cx - w * 0.5f, bottom - h);
    ImVec2 mx(cx + w * 0.5f, bottom);
    float round = h * 0.5f;
    ImVec2 c(mn.x + h * 0.5f, mn.y + h * 0.5f); // the medallion's centre

    // The card: a soft shadow, a navy pill, a gold hairline that breathes.
    if (unfold > 0.0f)
    {
      float ca = a * ui::Clamp01(unfold * 3.0f);
      for (int i = 3; i >= 1; --i)
      {
        float g = float(i) * 5.0f * s;
        dl->AddRectFilled(ImVec2(mn.x - g, mn.y - g + 6.0f * s), ImVec2(mx.x + g, mx.y + g + 6.0f * s),
                          ui::Fade(IM_COL32(0, 0, 0, 255), 0.09f * ca), round + g);
      }
      dl->AddRectFilled(mn, mx, ui::Fade(ui::kPanel, 0.96f * ca), round);
      float breathe = 0.5f + 0.5f * std::sin(now * 3.0f);
      dl->AddRect(mn, mx, ui::Fade(kGold, (0.35f + 0.35f * breathe) * ca), round, ImDrawFlags_None, 1.5f * s);

      dl->PushClipRect(mn, mx, true);
      // A shine runs over it once the card is open, and once more later.
      for (float at : {0.85f, 3.2f})
      {
        float k = Seg(t, at, 0.9f);
        if (k > 0.0f && k < 1.0f)
        {
          float bw = 90.0f * s;
          float x = mn.x - bw + (w + 2.0f * bw) * ui::EaseOutCubic(k);
          Shine(dl, x - bw * 0.5f, x + bw * 0.5f, mn.y, mx.y, 30.0f * s, ui::Fade(kGoldLight, 0.22f * ca));
        }
      }
      // The text slides out of the medallion.
      if (text_a > 0.0f)
      {
        float slide = (1.0f - ui::EaseOutCubic(Seg(t, 0.65f, 0.5f))) * 18.0f * s;
        float tx = mn.x + h + 10.0f * s - slide;
        float ta = a * text_a;
        ui::Caps(dl, FontKind::kLabel, ImVec2(tx, mn.y + 16.0f * s), ui::Fade(kGold, ta), caption);
        ui::Text(dl, FontKind::kHeading, ImVec2(tx, mn.y + 38.0f * s), ui::Fade(ui::kText, ta), info.label.c_str());
        // The gamerscore: a gold pill at the right of the caption line.
        ImVec2 ss = ui::TextSize(FontKind::kSmall, score);
        ImVec2 pmx(mx.x - round * 0.6f, mn.y + 16.0f * s + ss.y);
        ImVec2 pmn(pmx.x - ss.x - 16.0f * s, mn.y + 12.0f * s);
        float pr = (pmx.y - pmn.y) * 0.5f;
        dl->AddRectFilled(pmn, pmx, ui::Fade(kGold, 0.16f * ta), pr);
        dl->AddRect(pmn, pmx, ui::Fade(kGold, 0.6f * ta), pr, ImDrawFlags_None, 1.0f * s);
        ui::Text(dl, FontKind::kSmall, ImVec2(pmn.x + 8.0f * s, pmn.y + 2.0f * s), ui::Fade(kGoldLight, ta), score);
      }
      dl->PopClipRect();
    }

    // The burst as the medallion lands: a ring spreading out, sparks flying.
    float burst = Seg(t, 0.25f, 0.9f);
    if (burst > 0.0f && burst < 1.0f)
    {
      float e = ui::EaseOutCubic(burst);
      dl->AddCircle(c, R * (1.0f + 0.9f * e), ui::Fade(kGoldLight, (1.0f - burst) * 0.8f * a), 48,
                    (3.0f - 2.0f * burst) * s);
      std::minstd_rand rng(info.id * 2654435761u + 1);
      for (int i = 0; i < 10; ++i)
      {
        float ang = 6.2831853f * (float(i) + float(rng() % 100) / 100.0f) / 10.0f;
        float dist = R * (1.05f + (0.7f + float(rng() % 60) / 100.0f) * e);
        ImVec2 p(c.x + std::cos(ang) * dist, c.y + std::sin(ang) * dist);
        Sparkle(dl, p, (4.0f + float(rng() % 4)) * s * (1.0f - burst * 0.6f),
                ui::Fade(i % 2 ? kGold : kGoldLight, (1.0f - burst) * a));
      }
    }

    // The medallion: a pulsing glow, the gold rim, the icon inside, two arcs turning.
    float r = R * pop * (1.0f - out);
    if (r > 0.5f)
    {
      float glow = 0.5f + 0.5f * std::sin(now * 4.0f);
      for (int i = 3; i >= 1; --i)
      {
        dl->AddCircleFilled(c, r + float(i) * 4.0f * s, ui::Fade(kGold, (0.05f + 0.04f * glow) * a), 48);
      }
      dl->AddCircleFilled(c, r, ui::Fade(ui::Mix(ui::kPanel, kGold, 0.25f), a), 48);
      float ir = (r - 5.0f * s) * 0.72f;
      if (rex::ui::ImmediateTexture *icon = IconOf(info))
      {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(icon), ImVec2(c.x - ir, c.y - ir), ImVec2(c.x + ir, c.y + ir),
                            ImVec2(0, 0), ImVec2(1, 1), ui::Fade(ui::kWhite, a), ir * 0.25f);
      }
      else
      {
        ui::Check(dl, c, ir * 1.2f, 4.0f * s, ui::Fade(kGoldLight, a));
      }
      dl->AddCircle(c, r, ui::Fade(kGold, a), 48, 3.0f * s);
      dl->AddCircle(c, r - 5.0f * s, ui::Fade(kGoldLight, 0.35f * a), 48, 1.0f * s);
      float turn = now * 2.2f;
      for (float off : {0.0f, 3.14159f})
      {
        dl->PathArcTo(c, r + 4.0f * s, turn + off, turn + off + 0.9f, 16);
        dl->PathStroke(ui::Fade(kGoldLight, 0.8f * a), ImDrawFlags_None, 2.0f * s);
      }
    }
    return true;
  }

  // The toast slot (UI thread, in an ImGui frame): the queue's first toast,
  // then the next; false once the queue is empty.
  bool DrawToasts()
  {
    Toast toast;
    double now = ImGui::GetTime();
    bool ring = false;
    {
      std::lock_guard lock(g_toast_mutex);
      if (g_toasts.empty())
      {
        return false;
      }
      if (g_toasts.front().start < 0.0)
      {
        g_toasts.front().start = now;
        ring = true;
      }
      toast = g_toasts.front();
    }
    if (ring)
    {
      PlayUnlockSound();
    }
    if (DrawToast(toast, float(now - toast.start)))
    {
      return true;
    }
    std::lock_guard lock(g_toast_mutex);
    g_toasts.pop_front();
    return !g_toasts.empty();
  }

  // Any thread (the achievement manager's notification).
  void PushToast(const rex::system::AchievementInfo &info)
  {
    if (!REXCVAR_GET(ao2_achievement_notices))
    {
      return;
    }
    {
      std::lock_guard lock(g_toast_mutex);
      g_toasts.push_back({info});
    }
    ao2::menu::SetToast(DrawToasts);
  }

  // --- The achievements menu ---

  std::atomic<bool> g_menu_open = false;
  std::atomic<bool> g_menu_close = false; // the menu key, from any thread

  const char *SectionTitle(int section)
  {
    static const char *const kNames[] = {"STORY", "CAMPAIGN", "GEAR AND CO-OP", "WEAPONS",
                                         "COMBAT", "VERSUS", "SSC CHALLENGE"};
    static_assert(std::size(kNames) == std::size(kSections));
    return ao2::text::Tr(kNames[section]);
  }

  // "dd.mm.yyyy" in local time; empty without a time.
  std::string DateOf(uint64_t filetime)
  {
    if (!filetime)
    {
      return {};
    }
    FILETIME utc{DWORD(filetime), DWORD(filetime >> 32)}, local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
    {
      return {};
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%02u.%02u.%04u", unsigned(st.wDay), unsigned(st.wMonth), unsigned(st.wYear));
    return text;
  }

  // The menu (UI thread, inside an ImGui frame): a full screen overlay in the
  // look of the players window (coop_invite.cpp). The achievements by
  // section, the unlocked ones with their icon, score and date, the locked
  // ones dimmed (a secret one hides its name and icon). The wheel, the arrows,
  // Page Up / Down, Home and End scroll; Esc, the key and Close play it out.
  struct AchievementsMenu
  {
    static constexpr float kOpenTime = 0.35f;
    static constexpr float kCloseTime = 0.2f;

    struct Group
    {
      int section;
      std::vector<rex::system::AchievementInfo> items;
    };

    std::vector<Group> groups;
    double opened = -1.0;
    double closing = -1.0;
    float scroll = 0.0f, scroll_target = 0.0f;

    explicit AchievementsMenu(const std::vector<rex::system::AchievementInfo> &list)
    {
      for (int s = 0; s < static_cast<int>(std::size(kSections)); ++s)
      {
        Group group{s, {}};
        for (uint32_t id : kSections[s].ids)
        {
          if (!id)
          {
            break;
          }
          auto it = std::find_if(list.begin(), list.end(), [id](const auto &a) { return a.id == id; });
          if (it != list.end())
          {
            group.items.push_back(*it);
          }
        }
        if (!group.items.empty())
        {
          groups.push_back(std::move(group));
        }
      }
    }

    bool Draw()
    {
      namespace ui = ao2::ui;
      using ui::FontKind;
      auto *ks = rex::system::kernel_state();
      if (!ks)
      {
        g_menu_open = false;
        return false;
      }
      auto &am = ks->achievements();
      double now = ImGui::GetTime();
      if (opened < 0.0)
      {
        opened = now;
      }
      float a = ui::EaseOutCubic(float(now - opened) / kOpenTime);
      if (closing >= 0.0)
      {
        float c = float(now - closing) / kCloseTime;
        if (c >= 1.0f)
        {
          g_menu_open = false;
          return false;
        }
        a *= 1.0f - ui::EaseOutCubic(c);
      }
      bool live = closing < 0.0;
      float s = ui::Scale();

      int total = 0, got = 0;
      uint32_t score = 0, score_total = 0;
      for (const Group &g : groups)
      {
        for (const auto &info : g.items)
        {
          ++total;
          score_total += info.gamerscore;
          if (am.IsUnlocked(info.id))
          {
            ++got;
            score += info.gamerscore;
          }
        }
      }

      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##achievements", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar(2);
      if (live && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || g_menu_close.exchange(false)))
      {
        closing = now;
        live = false;
      }
      ImDrawList *dl = ImGui::GetWindowDrawList();

      // --- Backdrop and panel ---
      const float pad = 32.0f * s;
      const float pw = std::min(880.0f * s, vp->Size.x - 40.0f * s);
      const float ph = std::min(940.0f * s, vp->Size.y - 60.0f * s);
      ImVec2 center = vp->GetCenter();
      float x0 = center.x - pw * 0.5f - (1.0f - a) * 40.0f * s;
      float y0 = center.y - ph * 0.5f;
      float x1 = x0 + pw, y1 = y0 + ph;
      float cx0 = x0 + pad, cx1 = x1 - pad;
      dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                        ui::Fade(ui::kInk, 0.78f * a));
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kPanel, 0.96f * a));
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ui::Fade(ui::kWhite, 0.13f * a), 0.0f, ImDrawFlags_None, 1.0f * s);

      // --- Header: kicker, title, the counts, the progress ---
      float y = y0 + pad;
      ui::Caps(dl, FontKind::kLabel, ImVec2(cx0, y), ui::Fade(ui::kMuted, a), "ARMY OF TWO");
      y += ui::FontSize(FontKind::kLabel) + 6.0f * s;
      const char *title = ao2::text::Tr("ACHIEVEMENTS");
      ImVec2 title_size = ui::CapsSize(FontKind::kTitle, title);
      ui::Caps(dl, FontKind::kTitle, ImVec2(cx0, y), ui::Fade(ui::kAccentLight, a), title);
      {
        char status[64];
        std::snprintf(status, sizeof(status), "%d/%d  ·  %u/%u G", got, total, unsigned(score), unsigned(score_total));
        ImVec2 ss = ui::CapsSize(FontKind::kLabel, status);
        float chip_h = ss.y + 12.0f * s;
        ImVec2 cmx(cx1, y + (title_size.y + chip_h) * 0.5f);
        ImVec2 cmn(cmx.x - ss.x - 28.0f * s, cmx.y - chip_h);
        dl->AddRectFilled(cmn, cmx, ui::Fade(kGold, 0.12f * a));
        dl->AddRect(cmn, cmx, ui::Fade(kGold, 0.55f * a), 0.0f, ImDrawFlags_None, 1.0f * s);
        ui::Caps(dl, FontKind::kLabel, ImVec2(cmn.x + 14.0f * s, cmn.y + 6.0f * s), ui::Fade(kGoldLight, a), status);
      }
      y += title_size.y + 16.0f * s;
      float bar_h = 4.0f * s;
      dl->AddRectFilled(ImVec2(cx0, y), ImVec2(cx1, y + bar_h), ui::Fade(ui::kWhite, 0.10f * a));
      float frac = total ? float(got) / float(total) : 0.0f;
      dl->AddRectFilled(ImVec2(cx0, y), ImVec2(cx0 + (cx1 - cx0) * frac * ui::EaseOutCubic(a), y + bar_h),
                        ui::Fade(kGold, a));
      y += bar_h + 22.0f * s;

      // --- The list's area, and the footer under it ---
      const float kh = 24.0f * s;
      const float list_top = y;
      const float footer_y = y1 - pad - kh;
      const float list_bottom = footer_y - 16.0f * s;
      const float list_h = list_bottom - list_top;
      const float row_h = 76.0f * s, gap = 8.0f * s;
      const float head_h = ui::FontSize(FontKind::kLabel) + 14.0f * s;
      float content_h = 0.0f;
      for (const Group &g : groups)
      {
        content_h += head_h + float(g.items.size()) * (row_h + gap) + 14.0f * s;
      }
      float max_scroll = std::max(0.0f, content_h - list_h);
      if (live)
      {
        const ImGuiIO &io = ImGui::GetIO();
        scroll_target -= io.MouseWheel * 90.0f * s;
        float step = (row_h + gap);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        {
          scroll_target += step;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        {
          scroll_target -= step;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
        {
          scroll_target += list_h * 0.9f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
        {
          scroll_target -= list_h * 0.9f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
        {
          scroll_target = 0.0f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))
        {
          scroll_target = max_scroll;
        }
      }
      scroll_target = std::clamp(scroll_target, 0.0f, max_scroll);
      scroll = ui::Approach(scroll, scroll_target, 18.0f);

      const float sb_w = 4.0f * s;
      const float rx0 = cx0, rx1 = max_scroll > 0.0f ? cx1 - sb_w - 10.0f * s : cx1;
      dl->PushClipRect(ImVec2(x0 + 1.0f, list_top), ImVec2(x1 - 1.0f, list_bottom), true);
      y = list_top - scroll;
      for (const Group &g : groups)
      {
        int unlocked = 0;
        for (const auto &info : g.items)
        {
          unlocked += am.IsUnlocked(info.id) ? 1 : 0;
        }
        {
          const char *label = SectionTitle(g.section);
          ui::Caps(dl, FontKind::kLabel, ImVec2(rx0, y), ui::Fade(ui::kMuted, a), label);
          float lx = rx0 + ui::CapsSize(FontKind::kLabel, label).x + 10.0f * s;
          std::string n = std::to_string(unlocked) + "/" + std::to_string(g.items.size());
          ui::Caps(dl, FontKind::kLabel, ImVec2(lx, y), ui::Fade(ui::kAccent, a), n.c_str());
          lx += ui::CapsSize(FontKind::kLabel, n.c_str()).x + 10.0f * s;
          float ly = y + ui::FontSize(FontKind::kLabel) * 0.65f;
          dl->AddLine(ImVec2(lx, ly), ImVec2(rx1, ly), ui::Fade(ui::kWhite, 0.10f * a), 1.0f * s);
        }
        y += head_h;
        for (const auto &info : g.items)
        {
          if (y + row_h >= list_top && y <= list_bottom)
          {
            DrawRow(dl, info, am.IsUnlocked(info.id), am.GetUnlockTime(info.id), ImVec2(rx0, y), ImVec2(rx1, y + row_h),
                    a);
          }
          y += row_h + gap;
        }
        y += 14.0f * s;
      }
      dl->PopClipRect();

      if (max_scroll > 0.0f)
      {
        float tx = cx1 - sb_w;
        float thumb_h = std::max(30.0f * s, list_h * list_h / content_h);
        float thumb_y = list_top + (list_h - thumb_h) * (scroll / max_scroll);
        dl->AddRectFilled(ImVec2(tx, list_top), ImVec2(tx + sb_w, list_bottom), ui::Fade(ui::kWhite, 0.06f * a));
        dl->AddRectFilled(ImVec2(tx, thumb_y), ImVec2(tx + sb_w, thumb_y + thumb_h), ui::Fade(ui::kAccent, 0.7f * a));
      }

      // --- Footer ---
      dl->AddLine(ImVec2(cx0, footer_y - 8.0f * s), ImVec2(cx1, footer_y - 8.0f * s), ui::Fade(ui::kWhite, 0.08f * a),
                  1.0f * s);
      {
        const char *label = ao2::text::Tr("CLOSE");
        float label_w = ui::CapsSize(FontKind::kLabel, label).x;
        float key_w = std::max(kh, ui::CapsSize(FontKind::kLabel, "ESC").x + kh * 0.8f); // as ui::KeyCap
        float total_w = key_w + 8.0f * s + label_w;
        ImVec2 pos(cx1 - total_w, footer_y);
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##close", ImVec2(total_w, kh)) && live)
        {
          closing = now;
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ui::KeyCap(dl, pos, kh, "ESC", a);
        ui::Caps(dl, FontKind::kLabel,
                 ImVec2(pos.x + key_w + 8.0f * s, footer_y + (kh - ui::FontSize(FontKind::kLabel)) * 0.5f - 1.0f * s),
                 ui::Fade(hovered ? ui::kAccentLight : ui::kText, a), label);
      }
      ImGui::End();
      return true;
    }

    // One row in [mn, mx]; `a` is the menu's fade.
    static void DrawRow(ImDrawList *dl, const rex::system::AchievementInfo &info, bool unlocked, uint64_t when,
                        ImVec2 mn, ImVec2 mx, float a)
    {
      namespace ui = ao2::ui;
      using ui::FontKind;
      float s = ui::Scale();
      bool secret = !(info.flags & 0x8) && !unlocked;
      float h = mx.y - mn.y;
      dl->AddRectFilled(mn, mx, ui::Fade(unlocked ? kGold : ui::kWhite, (unlocked ? 0.07f : 0.035f) * a));
      dl->AddRect(mn, mx, ui::Fade(unlocked ? kGold : ui::kWhite, (unlocked ? 0.28f : 0.08f) * a), 0.0f,
                  ImDrawFlags_None, 1.0f * s);
      dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f * s, mx.y), ui::Fade(unlocked ? kGold : ui::kMuted, a));

      float isz = h - 24.0f * s;
      ImVec2 imn(mn.x + 16.0f * s, mn.y + 12.0f * s), imx(imn.x + isz, imn.y + isz);
      rex::ui::ImmediateTexture *icon = secret ? nullptr : IconOf(info);
      if (icon)
      {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(icon), imn, imx, ImVec2(0, 0), ImVec2(1, 1),
                            ui::Fade(ui::kWhite, (unlocked ? 1.0f : 0.35f) * a), isz * 0.2f);
      }
      else
      {
        dl->AddRectFilled(imn, imx, ui::Fade(ui::kWhite, 0.06f * a), isz * 0.2f);
        ImVec2 q = ui::TextSize(FontKind::kHeading, "?");
        ui::Text(dl, FontKind::kHeading, ImVec2(imn.x + (isz - q.x) * 0.5f, imn.y + (isz - q.y) * 0.5f),
                 ui::Fade(ui::kMuted, a), "?");
      }
      if (unlocked)
      {
        dl->AddRect(imn, imx, ui::Fade(kGold, 0.8f * a), isz * 0.2f, ImDrawFlags_None, 1.5f * s);
      }

      // The score pill and the date, at the right.
      char score[16];
      std::snprintf(score, sizeof(score), "%u G", info.gamerscore);
      ImVec2 ss = ui::TextSize(FontKind::kSmall, score);
      float pill_h = ss.y + 4.0f * s, pill_w = ss.x + 18.0f * s;
      ImVec2 pmn(mx.x - 16.0f * s - pill_w, mn.y + 12.0f * s), pmx(pmn.x + pill_w, pmn.y + pill_h);
      ImU32 tone = unlocked ? kGold : ui::kMuted;
      dl->AddRectFilled(pmn, pmx, ui::Fade(tone, 0.16f * a), pill_h * 0.5f);
      dl->AddRect(pmn, pmx, ui::Fade(tone, 0.6f * a), pill_h * 0.5f, ImDrawFlags_None, 1.0f * s);
      ui::Text(dl, FontKind::kSmall, ImVec2(pmn.x + 9.0f * s, pmn.y + 2.0f * s),
               ui::Fade(unlocked ? kGoldLight : ui::kMuted, a), score);
      if (unlocked)
      {
        std::string date = DateOf(when);
        if (!date.empty())
        {
          ImVec2 ds = ui::TextSize(FontKind::kSmall, date.c_str());
          ui::Text(dl, FontKind::kSmall, ImVec2(mx.x - 16.0f * s - ds.x, pmx.y + 8.0f * s), ui::Fade(ui::kMuted, a),
                   date.c_str());
        }
      }

      // The name and the description, clipped before the pill.
      float tx = imx.x + 16.0f * s;
      dl->PushClipRect(ImVec2(tx, mn.y), ImVec2(pmn.x - 12.0f * s, mx.y), true);
      const std::string &name = secret ? std::string(ao2::text::Tr("SECRET ACHIEVEMENT")) : info.label;
      ui::Text(dl, FontKind::kHeading, ImVec2(tx, mn.y + 12.0f * s), ui::Fade(unlocked ? ui::kText : ui::kMuted, a),
               name.c_str());
      const std::string &desc =
          !unlocked && !info.unachieved_description.empty() ? info.unachieved_description : info.description;
      ui::Text(dl, FontKind::kSmall, ImVec2(tx, mn.y + 14.0f * s + ui::FontSize(FontKind::kHeading)),
               ui::Fade(ui::kMuted, a), desc.c_str());
      dl->PopClipRect();
    }
  };
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
    static bool registered = false;
    if (!registered)
    {
      registered = true;
      ks->achievements().RegisterNotificationCallback(
          [](const rex::system::AchievementEvent &e) { PushToast(e.achievement); });
    }
    REXLOG_INFO("achievements: {} in '{}'", count, code);
    if (!cache_root.empty())
    {
      Export(cache_root, db);
    }
  }

  void ToggleMenu()
  {
    auto *ks = rex::system::kernel_state();
    if (!ks)
    {
      return;
    }
    if (g_menu_open.exchange(true))
    {
      g_menu_close = true; // open: the next frame plays it out
      return;
    }
    g_menu_close = false;
    auto menu = std::make_shared<AchievementsMenu>(ks->achievements().ListAchievements());
    ao2::menu::OpenWindow("achievements", [menu]() { return menu->Draw(); }, /*own_escape=*/true);
  }
} // namespace ao2::ach
