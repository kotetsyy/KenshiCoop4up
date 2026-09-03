# KenshiCoop4up

**Форк [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) — оригинального
кооп-мода для [Kenshi](https://lofigames.com/), созданного
[nhoral](https://github.com/nhoral).**

Весь мод — заслуга nhoral: архитектура, модель репликации, хуки в движок и
практически весь код принадлежат ему. Этот форк существует только чтобы нести
несколько правок сверху — см. [Что меняет форк](#что-меняет-форк). Лицензия
[AGPL-3.0](LICENSE), как и у оригинала.

🇷🇺 Русский (ниже) · [🇬🇧 English](#kenshicoop4up-english)

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
  на GitHub — в отдельном потоке — и ставит новую сборку, если она есть.
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

---

# KenshiCoop4up (English)

**A fork of [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) — the original
co-op mod for [Kenshi](https://lofigames.com/), created by
[nhoral](https://github.com/nhoral).**

All credit for the mod itself goes to nhoral: the architecture, the replication
model, the engine hooks and nearly all of the code are theirs. This fork exists
only to carry a few changes on top — see [What this fork changes](#what-this-fork-changes).
Licensed [AGPL-3.0](LICENSE), like the original.

[🇷🇺 Русский](#kenshicoop4up) · 🇬🇧 English (below)

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
  thread, and installs a newer build if one exists.
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
