# KenshiCoop4up

**A fork of [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) — the original
co-op mod for [Kenshi](https://lofigames.com/), created by
[nhoral](https://github.com/nhoral).**

All credit for the mod itself goes to nhoral: the architecture, the replication
model, the engine hooks and nearly all of the code are theirs. This fork exists
only to carry a few changes on top — see [What this fork changes](#what-this-fork-changes).
Licensed [AGPL-3.0](LICENSE), like the original.

🇬🇧 English (below) · [🇷🇺 Русский](#kenshicoop4up-русский)

---

Experimental **co-op multiplayer for Kenshi**, built as an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin.

One player hosts their world; friends connect (Steam P2P, LAN, or direct UDP)
and play their own squads inside it. The plugin replicates squads, NPCs, combat,
inventory and equipment, trades between the players' squads, items dropped on
the ground, base building and container contents, one shared money pool, game
speed, and more. Saves are coordinated: any save either player makes becomes one
shared save, streamed to both machines automatically.

> **Status: work in progress.** A hobby project under active development. Expect
> rough edges, desyncs, and crashes. Two players is the well-tested case; three
> and four are implemented but not validated in real sessions.

## What this fork changes

- **Self-update.** Every player must run the *same* DLL — the protocol version is
  a hard gate at handshake, so a single stale copy just reads as "it will not
  connect". The mod now checks a manifest on GitHub at startup, on its own
  thread, and installs a newer build if one exists. See
  [Automatic updates](#automatic-updates).
- **Loot crash fix.** Destroying an item while an inventory panel had it open
  freed memory the open window still pointed at, and the game died on its render
  thread. The reconcile now defers while a panel is open instead.
- **3–4 players.** Present in the code (host + up to 3 joins). Not validated in
  real sessions yet — treat it as untested.

Everything else is nhoral's work. If you are looking for the original,
supported mod, go to [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop).

## Install

You need Kenshi 1.0.65, [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847),
and — for the Steam transport — Steam running and online on every machine. That
is the whole network setup: no port forwarding, no router configuration, no IP
addresses.

**First install:** from the
[latest release](https://github.com/kotetsyy/KenshiCoop4up/releases/latest)
download all three files into `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — the plugin
- `RE_Kenshi.json` — tells RE_Kenshi to load that DLL (without it the mod never starts)
- `KenshiCoop.mod` — so Kenshi lists it in the Mods menu

Launch Kenshi and enable **KenshiCoop** in the Mods menu.

**Later updates:** if that folder already exists, only the DLL needs replacing
(the in-game updater does exactly that). `.mod` and `RE_Kenshi.json` almost
never change.

**Everyone must run the same build.** Mismatched versions do not connect.

## Connect in-game (F2)

The Co-op panel works at the **main menu** as well as in-game, so a joining
player does not need to load anything first.

1. Press **F2**.
2. **Type a nick.** The row says **Your nick** — click the box under it and
   type your name. That name is written onto the squad unit you play after you
   connect. Set it before going ONLINE.
3. **Swap Steam IDs.** Each player clicks **Copy my Steam ID** and sends it to
   the others. Copy the one you receive, then click **Paste friend's Steam ID**.
   The panel shows what it captured. Nick and Steam ID are remembered in
   `coop_config.json`, so a relaunch pre-fills the panel.
4. Leave **Transport** on **STEAM**.
5. **Host:** load a save or start a new game — the mod ships two-squad co-op
   starts — then set **Role: HOST** and toggle **Connection** to **ONLINE**.
6. **Join:** straight from the main menu, set **Role: JOIN** and go **ONLINE**.
   The host streams its world to you and you load into it.

**LAN / direct UDP:** set **Transport: UDP**. Steam ID rows hide. Type or paste
`ip:port` in **Host IP:port** (e.g. `192.168.1.10:27800`).

Each player controls their own squad: one squad tab per player, host runs squad
1, the first join squad 2, and so on. If your save has only one squad, split
some units into another squad tab in-game.

## Automatic updates

At startup the mod fetches a small manifest over HTTPS from this repository,
compares it against the version built into the DLL, and — if a newer build
exists — downloads it, verifies its SHA-256, and swaps it into place. The
running session keeps the old code; **the update takes effect the next time you
launch**, and the F2 panel says so.

It is on by default. To change or disable it, edit `coop_config.json` next to
the DLL:

```jsonc
"updateEnabled":   true,             // false = never check
"updateAutoApply": true,             // false = tell me, download nothing
"updateOwner":     "kotetsyy",       // point at your own fork if you prefer
"updateRepo":      "KenshiCoop4up"
```

**What this means for trust:** whoever controls the repository this points at
can put code on your machine. The download must come from GitHub over HTTPS and
must match the SHA-256 in the manifest, but that is a check on *transport*, not
on intent. If you would rather not have that, set `updateAutoApply` to `false`
(you still get told when a new version exists) or `updateEnabled` to `false`.

## Troubleshooting

- **"The co-op plugin has not started"** — RE_Kenshi did not load it. Check
  `<Kenshi>\RE_Kenshi_log.txt` for `KenshiCoop`; reinstalling RE_Kenshi usually
  fixes it.
- **No connection over Steam** — both Steams must be online (not offline mode),
  and each side must have pasted the *other* player's ID. Look for
  `[steam] session ... active=1` in `<Kenshi>\KenshiCoop_*.log`.
- **"protocol mismatch" in the log** — someone is on an older build. Everyone
  re-installs the same release, or lets the updater do it.

## Building

The plugin must be compiled with the **Visual C++ 2010 (v100) x64 toolset** (a
KenshiLib requirement). Full setup is in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

```bash
cmd //c scripts/build_plugin.cmd Release
```

A known quirk: MSBuild reports `MSB4018 ... mt.exe unexpectedly not a rooted
path` *after* a successful link. The DLL is already built — check for the file,
not the exit code.

Dependencies are fetched, not committed:

- KenshiLib + precompiled libs: clone
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  into `third_party/KenshiLib_deps/`
- ENet: clone [lsalzman/enet](https://github.com/lsalzman/enet) into
  `third_party/enet/enet/` and apply `third_party/enet/patches/`

Publishing a release (maintainers): build, then

```bash
powershell -ExecutionPolicy Bypass -File scripts/publish_release.ps1 -Notes "what changed"
```

It reads the version out of `src/netproto/Wire.h`, hashes the DLL, writes
`dist/UPDATE.txt`, and prints the two commands to run. Push the release asset
*before* the manifest — clients read the manifest first.

## Credits

- **[nhoral](https://github.com/nhoral) — author of KenshiCoop.** This fork is
  their work with a few changes on top.
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) — RE_Kenshi and
  KenshiLib, which make plugins like this possible
- [lsalzman/enet](https://github.com/lsalzman/enet) — UDP networking library
- [zeroit789](https://github.com/zeroit789) — the "Multiplayer (Wanderer x2)"
  co-op game start ([#15](https://github.com/nhoral/KenshiCoop/pull/15))
- Lo-Fi Games — Kenshi

## License

[AGPL-3.0](LICENSE), inherited from the original project. KenshiLib and
RE_Kenshi are GPLv3; this plugin links KenshiLib under GPLv3 section 13
(GPL/AGPL combination). Not affiliated with Lo-Fi Games. Non-commercial fan
project.

---

# KenshiCoop4up (Русский)

**Форк [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) — оригинального
кооп-мода для [Kenshi](https://lofigames.com/), созданного
[nhoral](https://github.com/nhoral).**

Весь мод — заслуга nhoral: архитектура, модель репликации, хуки в движок и
практически весь код принадлежат ему. Этот форк существует только чтобы нести
несколько правок сверху — см. [Что меняет форк](#что-меняет-форк). Лицензия
[AGPL-3.0](LICENSE), как и у оригинала.

[🇬🇧 English](#kenshicoop4up) · 🇷🇺 Русский (ниже)

---

Экспериментальный **кооператив для Kenshi**, сделанный как плагин к
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib).

Один игрок хостит свой мир, друзья подключаются (Steam P2P, LAN или прямой UDP)
и играют в нём своими отрядами. Синхронизируются отряды, NPC, бой, инвентарь и
экипировка, обмен между отрядами игроков, брошенные на землю предметы,
строительство и содержимое контейнеров, общий кошелёк, скорость игры и другое.
Сохранения общие: любой сейв, сделанный любым из игроков, становится единым и
передаётся на обе машины автоматически.

> **Статус: в разработке.** Любительский проект. Возможны шероховатости,
> рассинхроны и вылеты. Два игрока — проверенный случай; три и четыре
> реализованы, но в реальных сессиях не валидированы.

## Что меняет форк

- **Автообновление.** Все игроки обязаны запускать *одну и ту же* DLL: версия
  протокола жёстко проверяется при рукопожатии, поэтому одна устаревшая копия
  выглядит просто как «не коннектится». Теперь мод при старте проверяет манифест
  на GitHub — в отдельном потоке — и ставит новую сборку, если она есть. См.
  [Автообновление](#автообновление).
- **Фикс вылета на луте.** Удаление предмета при открытом окне инвентаря
  освобождало память, на которую окно продолжало ссылаться, и игра падала на
  своём рендер-потоке. Теперь применение откладывается, пока окно открыто.
- **3–4 игрока.** В коде есть (хост + до 3 подключений). В реальных сессиях пока
  не проверено — считайте непротестированным.

Всё остальное — работа nhoral. Если вам нужен оригинальный, поддерживаемый мод,
идите в [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop).

## Установка

Нужны Kenshi 1.0.65, [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) и —
для Steam-транспорта — запущенный Steam в сети на каждой машине. Это вся
сетевая настройка: без проброса портов, без настройки роутера, без IP-адресов.

**Первая установка:** из
[последнего релиза](https://github.com/kotetsyy/KenshiCoop4up/releases/latest)
скачайте все три файла в `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — сам плагин
- `RE_Kenshi.json` — говорит RE_Kenshi загрузить эту DLL (без него мод не стартует)
- `KenshiCoop.mod` — чтобы Kenshi показал мод в меню Mods

Запустите Kenshi и включите **KenshiCoop** в меню модов.

**Потом, при обновлении:** если папка уже есть, достаточно заменить DLL
(внутриигровой апдейтер так и делает). `.mod` и `RE_Kenshi.json` почти не
меняются.

**У всех должна быть одна и та же сборка.** Разные версии не соединяются.

## Подключение в игре (F2)

Панель работает и в **главном меню**, и в игре, так что подключающемуся не нужно
ничего предварительно загружать.

1. Нажмите **F2**.
2. **Введите ник.** Строка **Your nick** — кликните поле под ней и наберите
   имя. После входа оно ставится на юнит в вашем отряде. Задайте ник до ONLINE.
3. **Обменяйтесь Steam ID.** Каждый жмёт **Copy my Steam ID** и отправляет
   остальным. Полученный ID скопируйте и нажмите **Paste friend's Steam ID** —
   панель покажет, что распознала. Ник и Steam ID запоминаются в
   `coop_config.json`, поэтому при следующем запуске поля уже заполнены.
4. **Transport** оставьте на **STEAM**.
5. **Хост:** загрузите сейв или начните новую игру (в моде есть старты на два
   отряда), затем **Role: HOST** и **Connection** → **ONLINE**.
6. **Подключение:** прямо из главного меню — **Role: JOIN** и **ONLINE**. Хост
   передаст свой мир, и вы загрузитесь в него.

**LAN / прямой UDP:** поставьте **Transport: UDP**. Строки Steam ID скрываются.
В поле **Host IP:port** наберите или вставьте `ip:port` (например
`192.168.1.10:27800`).

Каждый игрок управляет своим отрядом: по вкладке отряда на игрока, у хоста отряд
1, у первого подключившегося — 2, и так далее. Если в сейве только один отряд,
разнесите юнитов по вкладкам прямо в игре.

## Автообновление

При старте мод забирает по HTTPS небольшой манифест из этого репозитория,
сравнивает с версией, вшитой в DLL, и если есть более новая сборка — скачивает
её, проверяет SHA-256 и подменяет файл. Текущая сессия продолжает работать на
старом коде; **обновление вступает в силу при следующем запуске**, и панель F2
об этом пишет.

По умолчанию включено. Изменить или выключить — в `coop_config.json` рядом с DLL:

```jsonc
"updateEnabled":   true,             // false = не проверять вообще
"updateAutoApply": true,             // false = только сообщить, ничего не качать
"updateOwner":     "kotetsyy",       // можно указать свой форк
"updateRepo":      "KenshiCoop4up"
```

**Что это значит с точки зрения доверия:** тот, кто владеет указанным
репозиторием, может доставить код на вашу машину. Загрузка обязана идти с
GitHub по HTTPS и совпасть с SHA-256 из манифеста, но это проверка *канала*, а
не намерений. Если так не хочется — поставьте `updateAutoApply` в `false` (о
новой версии всё равно сообщат) или `updateEnabled` в `false`.

## Если что-то не так

- **«The co-op plugin has not started»** — RE_Kenshi не загрузил плагин.
  Посмотрите `KenshiCoop` в `<Kenshi>\RE_Kenshi_log.txt`; обычно помогает
  переустановка RE_Kenshi.
- **Не соединяется по Steam** — оба Steam должны быть в сети (не в офлайн-режиме),
  и каждый должен вставить ID *другого* игрока. Ищите
  `[steam] session ... active=1` в `<Kenshi>\KenshiCoop_*.log`.
- **«protocol mismatch» в логе** — у кого-то старая сборка. Переустановите всем
  один релиз или дайте отработать автообновлению.

## Сборка

Собирать нужно **Visual C++ 2010 (v100) x64** — это требование KenshiLib. Полная
настройка в [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

```bash
cmd //c scripts/build_plugin.cmd Release
```

Известная особенность: MSBuild выдаёт `MSB4018 ... mt.exe unexpectedly not a
rooted path` **после** успешной компоновки. DLL при этом уже собрана — смотрите
на наличие файла, а не на код возврата.

Зависимости не хранятся в репозитории:

- KenshiLib и предсобранные библиотеки: склонируйте
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  в `third_party/KenshiLib_deps/`
- ENet: склонируйте [lsalzman/enet](https://github.com/lsalzman/enet) в
  `third_party/enet/enet/` и примените патчи из `third_party/enet/patches/`

Публикация релиза (для мейнтейнеров): собрать, затем

```bash
powershell -ExecutionPolicy Bypass -File scripts/publish_release.ps1 -Notes "что изменилось"
```

Скрипт берёт версию из `src/netproto/Wire.h`, считает хеш DLL, пишет
`dist/UPDATE.txt` и печатает две команды. Ассет релиза должен уехать **раньше**
манифеста — клиенты сначала читают манифест.

## Благодарности

- **[nhoral](https://github.com/nhoral) — автор KenshiCoop.** Этот форк —
  его работа с несколькими правками сверху.
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) — RE_Kenshi и
  KenshiLib, без которых такие плагины невозможны
- [lsalzman/enet](https://github.com/lsalzman/enet) — сетевая библиотека UDP
- [zeroit789](https://github.com/zeroit789) — кооп-старт «Multiplayer
  (Wanderer x2)» ([#15](https://github.com/nhoral/KenshiCoop/pull/15))
- Lo-Fi Games — Kenshi

## Лицензия

[AGPL-3.0](LICENSE), унаследована от оригинального проекта. KenshiLib и
RE_Kenshi под GPLv3; плагин линкуется с KenshiLib по GPLv3 §13 (сочетание
GPL/AGPL). Не аффилировано с Lo-Fi Games. Некоммерческий фанатский проект.
