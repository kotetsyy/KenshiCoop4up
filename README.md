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
скачайте `KenshiCoop-kit.zip` и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\`.
В архиве только четыре файла; их также можно скачать отдельно в `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — сам плагин
- `KenshiCoopUI.dll` — нативное окно F2, шрифты и статус сессии
- `RE_Kenshi.json` — говорит RE_Kenshi загрузить эту DLL (без него мод не стартует)
- `KenshiCoop.mod` — чтобы Kenshi показал мод в меню Mods

Запустите Kenshi и включите **KenshiCoop** в меню модов.
`coop_config.json` создаётся при сохранении настроек F2; при обновлении оставьте
свой конфиг на месте. `PROVENANCE.json` и лаунчеры не нужны для запуска.
Исходники доступны в этом репозитории по тегу соответствующего релиза;
инструкции сборки — в [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).


**Потом, при обновлении:** заменяйте **обе DLL из одной сборки** при закрытой
игре. Внутриигровой апдейтер проверяет SHA-256 обеих DLL и устанавливает пару
для следующего запуска. `.mod` и `RE_Kenshi.json` почти не меняются.

**У всех должна быть одна и та же сборка.** Разные версии не соединяются.

## Подключение в игре (F2)

Панель работает и в **главном меню**, и в игре, так что подключающемуся не нужно
ничего предварительно загружать.

1. Нажмите **F2**.
2. **Введите ник** в поле. Поддерживаются кириллица, выделение, редактирование
   и **Ctrl+V**. Поле ника занимает всю ширину строки, отдельной кнопки вставки нет.
3. Выберите **Хост** или **Клиент**, затем **Steam** или **Прямой IP (UDP)**.
4. **Steam, хост:** скопируйте свой Steam ID кнопкой окна и передайте клиенту.
   У хоста нет поля для чужого ID.
5. **Steam, клиент:** введите или вставьте Steam ID хоста, затем нажмите
   **«Подключиться»**. Хост нажимает **«Создать сессию»** и загружает свой мир.
6. **UDP, хост:** при необходимости введите номер порта
   (по умолчанию `27800`), затем создайте сессию.
7. **UDP, клиент:** введите или вставьте адрес хоста `ip:port`, например
   `192.168.1.10:27800`, затем подключитесь. Для подключения через интернет
   нужен доступный UDP-порт хоста; Steam-проброс здесь не используется.

**Tab** переключает поля; **Enter** в поле запускает выбранное подключение.
Пустой или неверный ник/адрес/порт блокирует запуск и показывает причину.
Пока поле имеет фокус, игровые клавиши отключены. Ник ограничен 63 байтами UTF-8;
слишком длинная вставка через кнопку отклоняется, а не обрезается незаметно.

Клиент может подключиться из главного меню: хост передаст свой мир.
Ник, выбранный режим, адрес и единый UDP-порт запоминаются в `coop_config.json`;
смена роли не возвращает старый порт. Во время сессии поля и переключатели
заблокированы. **F2, Esc, крестик и «Скрыть (F2)» не отключают сеть**;
для этого есть отдельная кнопка остановки. Справа показаны реальные этапы,
передача мира и игроки. **«Диагностика»** раскрывает подробности; при переполнении
используйте **«Копировать отчёт»**. Обновление мода имеет отдельный блок и не
меняет состояние подключения.

Каждый игрок управляет своим отрядом: по вкладке отряда на игрока, у хоста отряд
1, у первого подключившегося — 2, и так далее. Если в сейве только один отряд,
разнесите юнитов по вкладкам прямо в игре.

## Экспериментальная диагностика сети

Приватная ветка `debug/live-net-telemetry` собирается командой
`scripts\build_plugin.cmd Harness`: это оптимизированная сборка с диагностикой,
а не конфигурация VS Debug с отладочной CRT. Статистика находится в
сворачиваемом блоке **«Диагностика»** окна F2 и в **«Копировать отчёт»**;
лог содержит строки `[net-diag]` раз в секунду. Байты/пакеты — локальный
трафик ENet с повторами, не доказательство совпадения миров. Неизвестная
готовность мира и отсутствующие измерения не выдаются за успешную синхронизацию.
Ничего не выгружается автоматически.

Для теста отключите автообновление (`"updateEnabled": false` в
`coop_config.json`), иначе публичный релиз может заменить диагностическую DLL.
Участникам теста давайте доступ к исходникам приватного репозитория согласно
AGPL-3.0. Экспериментальные коммиты остаются в приватной ветке; только
проверенное исправление переносится отдельным коммитом в публичную `main`.


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
[latest release](https://github.com/kotetsyy/KenshiCoop4up/releases/latest),
download `KenshiCoop-kit.zip` and copy its `KenshiCoop` folder into `<Kenshi>\mods\`.
The archive contains only four files, also available individually for `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — the plugin
- `KenshiCoopUI.dll` — the native F2 window, fonts and session status
- `RE_Kenshi.json` — tells RE_Kenshi to load that DLL (without it the mod never starts)
- `KenshiCoop.mod` — so Kenshi lists it in the Mods menu

Launch Kenshi and enable **KenshiCoop** in the Mods menu.
F2 creates `coop_config.json` when saving settings; preserve your config on
updates. Neither `PROVENANCE.json` nor a launcher is required at runtime.
Sources are available in this repository at the matching release tag;
build instructions are in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).


**Later updates:** replace **both DLLs from the same build**, with the game
closed. The in-game updater verifies both SHA-256 hashes and installs the pair
for the next launch. `.mod` and `RE_Kenshi.json` almost never change.

**Everyone must run the same build.** Mismatched versions do not connect.

## Connect in-game (F2)

The Co-op panel works at the **main menu** as well as in-game, so a joining
player does not need to load anything first.

1. Press **F2**.
2. **Type your nick** in the field. Cyrillic, selection, editing and **Ctrl+V**
   are supported. The nickname field uses the full row width, without a Paste button.
3. Choose **Host** or **Client**, then **Steam** or **Direct IP (UDP)**.
4. **Steam host:** copy your Steam ID using the window's button and send it to
   the client. The host has no field for a friend's ID.
5. **Steam client:** type or paste the host's Steam ID and click **Connect**.
   The host clicks **Create session** and loads their world.
6. **UDP host:** optionally enter a port number (default `27800`), then create
   the session.
7. **UDP client:** type or paste the host's `ip:port`, for example
   `192.168.1.10:27800`, then connect. Internet UDP requires a reachable host
   port; Steam's NAT traversal is not used for this transport.

**Tab** moves between fields; **Enter** starts the selected connection.
Empty or invalid fields prevent a start and show the reason. Game controls
are suppressed while editing. Nicks are limited to 63 UTF-8 bytes.

Clients can connect from the main menu and receive the host's world. Nick,
role, endpoint and one shared UDP port are remembered in `coop_config.json`;
switching roles does not restore an old port. Fields and selectors are locked
during a session. **F2, Esc, X and Hide never disconnect**; use the separate
stop/disconnect action. The right column shows real milestones, transfer
progress and players. **Diagnostics** expands details; **Copy report** includes
overflowing lines. Mod updates have a separate section, not a connection status.

Each player controls their own squad: one squad tab per player, host runs squad
1, the first join squad 2, and so on. If your save has only one squad, split
some units into another squad tab in-game.

## Experimental network diagnostics

Build the private `debug/live-net-telemetry` branch with
`scripts\build_plugin.cmd Harness`. This optimized diagnostic pair uses **private
protocol 63**; the ordinary released pair remains on protocol 59. Host and join
must both use the same build pair. This private build disables public auto-updates
even if a tester's `coop_config.json` still has `"updateEnabled": true`.

Local ENet rates appear in the collapsible **Diagnostics** block and **Copy
report**, not a separate always-on overlay. Once a JOIN completes WELCOME, the
debug DLL forwards selected `[audit]`, `[inv]`, `[net-diag]`, `[save]` and related
diagnostic lines to the HOST over the existing ENet connection, on the bulk
channel. The host writes them as `[remote-join id=N clientMs=...] ...` beside its
own entries in `KenshiCoop_host.log`. The join retains its full
`KenshiCoop_join.log`. Forwarding is limited to eight lines per second with a
64-line in-memory queue; `[diag-relay] dropped=N` reports overflow. Save transfer
can delay the bulk channel. If a save transfer fails, inspect the forwarded
`[save] XFER-FAILED` and preceding `[save]` errors; the full join log can contain
additional detail. These are observations from two machines, **not** an automatic
verdict that their worlds match.

This chat has no direct connection to a tester's PC: live analysis means reading
the combined log on the host machine during a session. There is no separate
HTTP upload or outside log collector. With Steam transport, the existing game
connection may use Valve's relay. Logs may contain player names and game data;
share them privately. Give testers access to the private source under AGPL-3.0.
Experimental commits stay on the private branch; move only verified fixes to
public `main` in separate commits.


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
