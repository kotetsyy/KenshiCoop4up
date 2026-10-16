# KenshiCoop4up

**KenshiCoop4up — самостоятельно развиваемая и поддерживаемая версия
кооп-мода для [Kenshi](https://lofigames.com/), основанная на
[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop).**

Первоначальный автор KenshiCoop — [nhoral](https://github.com/nhoral):
оригинальная архитектура, модель репликации, хуки в движок и исходный код —
его работа. [kotetsyy](https://github.com/kotetsyy) сопровождает производную
версию, её изменения и релизы, а не является автором оригинального мода.
См. [Разработка KenshiCoop4up](#разработка-kenshicoop4up).
Лицензия [AGPL-3.0](LICENSE) сохранена от оригинального проекта.

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

> **Публичный v0.2.0 — протокол 80.** Steam и прямой IP используют
> GameNetworkingSockets (GNS), без ENet и автоматического переключения транспорта.
> Переход с 0.1.x требует ручной установки полного комплекта.
> Steam P2P между двумя реальными аккаунтами ещё не сертифицирован;
> реальные сессии на 3–4 игроков и устранение всех рассинхронов не подтверждены.

> **Локальная v0.2.1 ещё не опубликована:** UDP подключается только по IP:port,
> без ключей и приглашений. Release — протокол **82**, Harness — **83**,
> native ABI3/UI API4. Переход с v0.2.0 — вручную полным комплектом на всех машинах;
> старый updater не принимает новый ABI. Доступ к порту открыт без проверки личности.

## Разработка KenshiCoop4up

- **Автообновление.** Все игроки обязаны использовать одну версию. После ручного
  перехода на v0.2.0 мод проверяет манифест GitHub в отдельном потоке и обновляет
  все четыре DLL одной сборки, включая UI, для следующего запуска.
- **Фикс вылета на луте.** Удаление предмета при открытом окне инвентаря
  освобождало память, на которую окно продолжало ссылаться, и игра падала на
  своём рендер-потоке. Теперь применение откладывается, пока окно открыто.
- **Общий замок сундука.** Состояние замка передаётся вместе с изменениями
  дверей: разблокировка клиентом видна хосту и наоборот. Хост передаёт текущие
  замки при подключении; неизменившиеся состояния не пересылаются по таймеру,
  чтобы старый повтор не отменял новую разблокировку.
- **Подбор без окна для дублирования.** Предмет из чужого инвентаря нельзя
  выбросить, использовать или передать дальше до решения его владельца.
  Подтверждение сопровождается текущим содержимым общего слота, поэтому
  отклонённый подбор не оставляет копию до следующего периодического обновления.
- **Обмен без потери рюкзака.** Пока рюкзак держат курсором или передача ждёт
  подтверждения, неполный снимок не удаляет его копию у другого игрока.
  После завершения обмена отправляется полный инвентарь, даже если периодическая
  синхронизация отключена. Рюкзак с неподтверждённым предметом внутри нельзя
  передать дальше; откат возвращает именно переданный экземпляр.
- **Синхронизация без ложного захвата курсором.** В `v0.1.31` служебное
  перемещение экипировки при применении снимка больше не считается началом
  пользовательского обмена и не блокирует дальнейшее обновление инвентаря.
- **Один владелец персонажа после передачи.** Передача в отряд клиента снимает
  публикацию его собственного состояния и здоровья у хоста. Различающиеся
  локальные ID не ломают цели боя NPC и применение здоровья. Служебная смена
  отряда распознаётся по фактической паре ID «до → после», включая serial,
  и не отправляется обратно как действие игрока. Возврат персонажа в свой
  отряд сохраняет управление, даже если там находятся персонажи другого игрока.
  Если тело ещё не загружено, отложенное создание сохраняет назначенного
  владельца и связывает новый локальный ID с сетевым.
- **3–4 игрока.** В коде есть (хост + до 3 подключений). В реальных сессиях пока
  не проверено — считайте непротестированным.

Потеря предметов при прямом обмене, разные товары торговцев, рассинхроны
времени и координат, кровати на втором этаже и боевых анимаций не следует
считать полностью устранёнными в v0.2.0. Также остаются проблемы окружения,
экипировки и NPC; исправление торговли не означает исправление всех рассинхронов.

Направления разработки — устранение этих рассинхронов и потерь предметов,
улучшение диагностики и проверка сценариев подключения, загрузки и передачи
персонажей; проверка реальных сессий на 3–4 игроков остаётся отдельной задачей.
Ручная сборка, проверки и подготовка релизов описаны в
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

## Установка

Нужны Kenshi 1.0.65, [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) и —
для Steam-транспорта — запущенный Steam в сети на каждой машине и поддерживаемые
интерфейсы игры. Ручной список разрешённых SteamID не нужен. Реальный
двухаккаунтный P2P ещё не сертифицирован; ошибка Steam не включает UDP автоматически.

**Публичная установка v0.2.0 (протокол 80):** скачайте **`KenshiCoop-kit.zip`**
из [последнего релиза](https://github.com/kotetsyy/KenshiCoop4up/releases/latest).
При закрытой игре распакуйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов.
Комплект содержит ровно шесть файлов для `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — сам плагин
- `KenshiCoopUI.dll` — нативное окно F2, шрифты и статус сессии
- `KenshiCoopNetSteam.dll` — native Steamworks P2P, без скрытого UDP fallback
- `KenshiCoopNetUDP.dll` — authenticated direct UDP, без Steam API/relay
- `RE_Kenshi.json` — говорит RE_Kenshi загрузить эту DLL (без него мод не стартует)
- `KenshiCoop.mod` — чтобы Kenshi показал мод в меню Mods

Запустите Kenshi и включите **KenshiCoop** в меню модов.
`coop_config.json` создаётся при сохранении настроек F2; при обновлении оставьте
свой конфиг и сейвы на месте.
Исходники доступны в этом репозитории по тегу соответствующего релиза;
инструкции сборки — в [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).


**Первый переход с 0.1.x на v0.2.0 — только вручную:** установите полный
шестифайловый комплект при закрытой игре, сохранив конфиг и сейвы. Старый updater
не поддерживает новую схему. Затем новый updater обновляет все **четыре DLL
одной сборки**, включая `KenshiCoopUI.dll`, транзакционно для следующего запуска.
`KenshiCoop.mod` и `RE_Kenshi.json` устанавливаются из полного комплекта.

**У всех игроков должна быть одна версия.** v0.2.0 несовместим с 0.1.x.
Публичный Release использует протокол **80**, отдельный приватный Harness —
**81**, с отключённым автообновлением; эти сборки не соединяются друг с другом.
Хост держит симуляцию на паузе до загрузки мира всеми подключёнными клиентами:
READY подтверждает текущий `LOAD_GO` и полное согласование проверяемых участков,
а не просто окончание передачи файлов.
Пауза не заменяет выбранную игроками скорость или пользовательскую паузу.
Завершение сохранения мира для подключения больше не сбрасывает паузу хоста:
после READY игра продолжится только если пользовательская пауза снята.
Этот барьер работает при включённой синхронизации сейвов, загрузки, скорости
и времени. Сеть работает в отдельном native worker; целевая частота
снимков сущностей — **100 Гц** (10 мс, монотонный QPC-таймер). Свежие состояния
по-прежнему захватываются игровым потоком, поэтому их частота зависит от FPS.
Ротация дальних NPC остаётся на 50 мс; остальные каналы не переводятся на 100 Гц.
Это не гарантия пинга 1 мс; повышение частоты снимков увеличивает сетевую нагрузку.

В `v0.1.27` исправлены повторный лут и гонка за последним предметом: перенос
подтверждается только для реально забранного количества, отказ откатывает
временную клиентскую копию. Передача рюкзака включает его содержимое.
Загрузка уже лежащих в мире вещей больше не объявляется новым выбрасыванием.
Товар торговца синхронизируется из полок его магазина, включая разные чертежи,
а не только из инвентаря NPC. Для текущей версии всем нужен полный комплект v0.2.0.

## Подключение в игре (F2)

Панель работает и в **главном меню**, и в игре, так что подключающемуся не нужно
ничего предварительно загружать.

Ниже описана локальная v0.2.1. Опубликованный v0.2.0 ещё требует UDP-приглашения.

1. Нажмите **F2**.
2. **Введите ник** в поле. Поддерживаются кириллица, выделение, редактирование
   и **Ctrl+V**. Поле ника занимает всю ширину строки, отдельной кнопки вставки нет.
3. Выберите **Хост** или **Клиент**, затем **Steam** или **Прямой IP (UDP)**.
4. **Steam, хост:** создайте сессию и поделитесь своим SteamID64 с гостями.
   Ручное добавление SteamID гостей в список разрешённых не требуется.
5. **Steam, клиент:** введите SteamID64 хоста и подключитесь; хост создаёт сессию
   и загружает свой мир. Native-допуск не равен завершённой загрузке/READY.
6. **UDP, хост:** выберите порт (по умолчанию `27800`) и создайте сессию.
7. **UDP, клиент:** введите числовой IPv4/IPv6 адрес с портом, например
   `192.168.1.10:27800` или `[2001:db8::10]:27800`, и подключитесь.
   Ключи, пароли и отдельные приглашения не нужны. Для интернета нужен доступный
   порт хоста; автоматического обхода NAT/CGNAT нет. Транспорт шифруется, но не
   удостоверяет личность: к свободному месту может подключиться любой, кто достиг порта.

**Tab** переключает поля; **Enter** в поле запускает выбранное подключение.
Пустой или неверный ник/адрес/порт блокирует запуск и показывает причину.
Пока поле имеет фокус, игровые клавиши отключены. Ник ограничен 63 байтами UTF-8;
слишком длинная вставка отклоняется, а не обрезается незаметно.

Клиент может подключиться из главного меню: хост передаст свой мир.
Ник, выбранный режим, адрес и единый UDP-порт запоминаются в `coop_config.json`;
смена роли не возвращает старый порт. Во время сессии режим, способ подключения
и адрес заблокированы; ник можно менять. Вместимость — **4 игрока всего: хост и
до 3 друзей**. **F2, Esc, крестик и «Скрыть (F2)» не отключают сеть**.
Остановка хоста с подключёнными гостями, в том числе ещё загружающими мир,
требует подтверждения; по умолчанию выбрана **«Отмена»**, Esc также отменяет.
Один хост и клиент отключаются без дополнительного диалога.

Справа всегда видны состояние, этапы и игроки. Установленное соединение не
означает готовность мира; неизвестная готовность явно помечена.
**«Диагностика»** открывает отдельное перемещаемое окно со своей прокруткой,
не заменяя список игроков. **«Копировать отчёт»** экспортирует полный текст,
обезличивая имена, Steam ID и адреса, но сохраняя версии, состояния, ошибки,
порты и измерения. Явная кнопка копирования Steam ID передаёт настоящий ID.
Обновление мода имеет отдельный блок и не меняет состояние подключения.

Кнопка **Discord** в нижней строке, перед «Диагностикой», открывает
[сервер сообщества](https://discord.gg/UYcNPMrtup) в браузере по умолчанию.
Она не запускает и не отключает игровую сессию.

Каждый игрок управляет своим отрядом: по вкладке отряда на игрока, у хоста отряд
1, у первого подключившегося — 2, и так далее. Если в сейве только один отряд,
разнесите юнитов по вкладкам прямо в игре.

## Экспериментальная диагностика сети

Публичные исходники включают конфигурацию Harness:
`scripts\build_plugin.cmd Harness` собирает согласованную пару
`KenshiCoop.dll` и `KenshiCoopUI.dll`. Это оптимизированная сборка с
`KENSHICOOP_NET_DIAG`, а не конфигурация VS Debug с отладочной CRT.
Локальные исходники v0.2.1 используют Release **82** / Harness **83**;
опубликованный v0.2.0 остаётся на протоколах **80** / **81**.
Все участники должны использовать все четыре DLL из одной сборки;
Release и Harness не соединяются друг с другом. Harness принудительно отключает
публичный апдейтер, даже если в `coop_config.json` стоит `"updateEnabled": true`.
Инструкции сборки и проверок — в [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

Статистика находится в отдельном перемещаемом окне **«Диагностика»**
со своей прокруткой и в **«Копировать отчёт»**; Harness пишет строки
`[net-diag]` раз в секунду. Байты/пакеты — локальный native-трафик,
не доказательство совпадения миров. Неизвестная готовность мира и
отсутствующие измерения не выдаются за успешную синхронизацию.

После WELCOME Harness передаёт локальный лог клиента хосту по существующему
native-соединению, на канале массовых данных. Хост сохраняет его в
`KenshiCoop_join_N_mirror.log`, отдельно от своего `KenshiCoop_host.log`;
клиент сохраняет `KenshiCoop_join.log`. Передача ограничена восемью порциями
до 960 байт в секунду; оставшиеся байты хранятся на диске, а передача сейва
может задерживать доставку. Release не пересылает клиентские логи.

Логи остаются на машинах участников; отдельной HTTP-выгрузки или внешнего
сборщика нет. При Steam-транспорте игровое соединение может использовать
ретранслятор Valve. Логи могут содержать имена игроков и игровые данные —
делитесь ими приватно. При передаче экспериментальной сборки предоставляйте
участникам соответствующие исходники согласно AGPL-3.0, включая изменения
относительно публичного тега, если они есть.


## Благодарности

- **[nhoral](https://github.com/nhoral) — первоначальный автор KenshiCoop:**
  оригинальная архитектура, репликация, хуки в движок и исходный код.
- [kotetsyy](https://github.com/kotetsyy) — сопровождение, изменения и релизы
  производного проекта KenshiCoop4up
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) — RE_Kenshi и
  KenshiLib, без которых такие плагины невозможны
- [ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) — native transport
- [zeroit789](https://github.com/zeroit789) — кооп-старт «Multiplayer
  (Wanderer x2)» ([#15](https://github.com/nhoral/KenshiCoop/pull/15))
- Lo-Fi Games — Kenshi

## Лицензия

[AGPL-3.0](LICENSE), унаследована от оригинального проекта. KenshiLib и
RE_Kenshi под GPLv3; плагин линкуется с KenshiLib по GPLv3 §13 (сочетание
GPL/AGPL). Не аффилировано с Lo-Fi Games. Некоммерческий фанатский проект
по замыслу авторов; это не дополнительное ограничение прав по лицензии.

---

# KenshiCoop4up (English)

**KenshiCoop4up is an independently developed and maintained co-op mod for
[Kenshi](https://lofigames.com/), based on
[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop).**

[nhoral](https://github.com/nhoral) is KenshiCoop's original author: the original
architecture, replication model, engine hooks and source code are their work.
[kotetsyy](https://github.com/kotetsyy) maintains this derivative, its changes
and releases, and is not the author of the original mod.
See [KenshiCoop4up development](#kenshicoop4up-development).
The original project's [AGPL-3.0](LICENSE) license is retained.

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
> rough edges, desyncs, and crashes. Two players is the tested case; three
> and four are implemented but not validated in real sessions.

> **Public v0.2.0 — protocol 80.** Steam and direct IP use
> GameNetworkingSockets (GNS), without ENet or automatic transport fallback.
> Migrating from 0.1.x requires manual installation of the complete kit.
> Actual two-account Steam P2P is not yet certified; real 3–4-player sessions
> and resolution of all desynchronization reports remain unverified.

> **Local v0.2.1 is not published:** direct UDP uses only IP:port, without keys or
> invitations. Release protocol **82**, Harness **83**, native ABI3/UI API4.
> Install the full matching kit manually on every machine when migrating from
> v0.2.0; its updater rejects the new ABI. Reachable peers are not authenticated.

## KenshiCoop4up development

- **Self-update.** All players must use the same version. After the manual
  v0.2.0 migration, the mod checks GitHub's manifest on a separate thread and
  updates all four DLLs from one build, including the UI, for the next launch.
- **Loot crash fix.** Destroying an item while an inventory panel had it open
  freed memory the open window still pointed at, and the game died on its render
  thread. The reconcile now defers while a panel is open instead.
- **Shared chest locks.** Container-lock changes use the reliable door channel,
  so an unlock on either peer reaches the other. The host sends current locks
  when a peer connects; unchanged states are not periodically retransmitted,
  preventing a stale resend from undoing a newer unlock.
- **No speculative-pickup duplication window.** Items taken from another owner's
  inventory cannot be dropped, consumed or moved onward before its verdict.
  The verdict follows the current authoritative inventory snapshot, so a rejected
  pickup does not leave a phantom until the next periodic refresh.
- **Backpack exchange conservation.** A bag held by the cursor or awaiting a
  transfer verdict is not deleted by an incomplete peer snapshot. Settlement
  sends the full inventory even with periodic inventory publishing disabled.
  A bag containing a provisional item cannot be transferred onward; rollback
  returns the actual transferred bag rather than another copy of its template.
- **Snapshot moves are not cursor moves.** In `v0.1.31`, equipment repositioning
  during snapshot reconciliation no longer looks like a user-initiated transfer
  or blocks subsequent inventory updates.
- **One character author after handoff.** Moving a character into the Client's
  squad stops the Host from publishing it as its own or authoring its medical
  state. Native/wire identity differences are respected for NPC combat targets
  and health application. Replication-induced squad moves use the actual full
  before/after native identities, including changed serials, rather than being
  echoed as user actions. Returning to an owned mixed squad retains control.
  If the body is not loaded yet, deferred creation retains the assigned owner
  and binds the new native identity to its wire identity.
- **3–4 players.** Present in the code (host + up to 3 joins). Not validated in
  real sessions yet — treat it as untested.

New reports of direct-trade item loss, differing merchant stock, time and position
desynchronization, upstairs beds and combat animations should not be considered
fully resolved in v0.2.0. Environment, outfit and NPC issues also remain;
the merchant fix does not resolve every desynchronization.

Development focuses on resolving these desyncs and item losses, improving
diagnostics, and checking connection, world-loading and character-handoff
scenarios; validation of real 3–4-player sessions remains a separate task.
The manual build, checks and release preparation process is documented in
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

## Install

You need Kenshi 1.0.65, [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847),
and — for Steam — online Steam on each machine and supported game interfaces.
No manual SteamID allow-list is required. Actual two-account P2P is not yet
certified; Steam failures do not automatically select UDP.

**Public v0.2.0 installation (protocol 80):** download **`KenshiCoop-kit.zip`**
from the [latest release](https://github.com/kotetsyy/KenshiCoop4up/releases/latest).
With the game closed, extract its `KenshiCoop` folder into `<Kenshi>\mods\`,
replacing files. The kit contains exactly six files for `<Kenshi>\mods\KenshiCoop\`:

- `KenshiCoop.dll` — the plugin
- `KenshiCoopUI.dll` — the native F2 window, fonts and session status
- `KenshiCoopNetSteam.dll` — native Steamworks P2P, no hidden UDP fallback
- `KenshiCoopNetUDP.dll` — authenticated direct UDP, no Steam API/relay
- `RE_Kenshi.json` — tells RE_Kenshi to load that DLL (without it the mod never starts)
- `KenshiCoop.mod` — so Kenshi lists it in the Mods menu

Launch Kenshi and enable **KenshiCoop** in the Mods menu.
F2 creates `coop_config.json` when saving settings; preserve your config and
saves when updating.
Sources are available in this repository at the matching release tag;
build instructions are in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).


**First migration from 0.1.x to v0.2.0 is manual only:** install the complete
six-file kit with the game closed, preserving config and saves. Legacy updaters
do not support the new manifest. Thereafter the new updater transactionally
replaces all **four DLLs from one build**, including `KenshiCoopUI.dll`, for the
next launch. Install `KenshiCoop.mod` and `RE_Kenshi.json` from the complete kit.

**All players must use the same version.** v0.2.0 is incompatible with 0.1.x.
Public Release uses protocol **80**; the separate private Harness uses **81**
with updates disabled. Release and Harness cannot connect to each other.

## Connect in-game (F2)

The Co-op panel works at the **main menu** as well as in-game, so a joining
player does not need to load anything first.

These steps describe local v0.2.1; published v0.2.0 still requires UDP invitations.

1. Press **F2**.
2. **Type your nick** in the field. Cyrillic, selection, editing and **Ctrl+V**
   are supported. The nickname field uses the full row width, without a Paste button.
3. Choose **Host** or **Client**, then **Steam** or **Direct IP (UDP)**.
4. **Steam host:** create the session and share your SteamID64 with guests.
   No manual guest SteamID allow-list is required.
5. **Steam client:** enter the host SteamID64 and connect; host creates the session
   and loads its world. Native admission is not gameplay handshake/world READY.
6. **UDP host:** select a port (default `27800`) and create the session.
7. **UDP client:** enter a numeric IPv4/IPv6 endpoint such as `192.168.1.10:27800`
   or `[2001:db8::10]:27800`, then connect. No keys, passwords or invitations.
   Internet UDP needs a reachable host port; there is no automatic NAT/CGNAT bypass.
   Transport is encrypted but identities are unauthenticated: any reachable peer
   can occupy a free seat.

**Tab** moves between fields; **Enter** starts the selected connection.
Empty or invalid fields prevent a start and show the reason. Game controls
are suppressed while editing. Nicks are limited to 63 UTF-8 bytes.

Clients can connect from the main menu and receive the host's world. Nick,
role, endpoint and one shared UDP port are remembered in `coop_config.json`;
switching roles does not restore an old port. Role, transport and endpoint are
locked during a session; the nickname remains editable. Capacity is **4 players
total: the host and up to 3 friends**. **F2, Esc, X and Hide never disconnect**.
Stopping a host with accepted guests, including guests still loading the world,
requires confirmation. **Cancel** is the default; Esc also cancels. Solo hosts
and clients retain direct disconnection.

The right column always shows state, milestones and players. Network connection
does not imply world readiness; unknown readiness is explicitly labelled.
**Diagnostics** opens a separate movable window with its own scrolling, without
replacing the roster. **Copy report** exports the full text with participant
names, Steam IDs and addresses anonymized, preserving versions, states, errors,
ports and measurements. Explicit Steam ID copying still shares the real ID.
Mod updates have a separate section, not a connection status.

The **Discord** button in the footer, before Diagnostics, opens the
[community server invite](https://discord.gg/UYcNPMrtup) in your default browser.
It does not start or disconnect the game session.

Each player controls their own squad: one squad tab per player, host runs squad
1, the first join squad 2, and so on. If your save has only one squad, split
some units into another squad tab in-game.

## Experimental network diagnostics

The public sources include the Harness configuration:
`scripts\build_plugin.cmd Harness` builds a matching `KenshiCoop.dll` and
`KenshiCoopUI.dll` pair. It is an optimized build with `KENSHICOOP_NET_DIAG`,
not VS Debug with the debug CRT. Public Release v0.2.0 uses protocol **80**;
the private diagnostic Harness uses protocol **81**.
Every participant must use all four DLLs from the same build;
Release and Harness cannot connect to each other. Harness forcibly
disables the public updater even if `coop_config.json` has `"updateEnabled": true`.
See [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md) for build and check instructions.

Both configurations pause the host simulation until every connected join reports
the READY id for its current `LOAD_GO` after the new world becomes live, including
a join connecting from the title screen. The temporary hold does not replace the
speed/pause vote and requires save, load, speed and time sync to remain enabled.
Both configurations use a dedicated native network worker and target 100 Hz entity
snapshots (10 ms, paced by the monotonic QPC clock). Fresh captures still come
from the game thread and depend on its frame rate. The 50 ms distant-NPC slice
rotation and other channel budgets are unchanged. This is not a 1 ms ping
guarantee; the higher snapshot rate increases network load.

v0.1.27 fixes repeated loot and competing takes of the last item: a transfer is
acknowledged only for the quantity actually removed, and rejection rolls back
the provisional client copy. Backpack transfers include their contents.
Streaming an existing ground item no longer announces a fresh drop. Merchant
stock comes from the actual shop furniture, including distinct blueprints,
not just the NPC's character inventory. All peers now need the complete v0.2.0 kit.

Local native transport rates appear in the separate movable **Diagnostics** window with
its own scrolling and in **Copy report**. Harness logs `[net-diag]` lines once
per second. Rates are not proof that worlds match;
unknown world readiness and missing measurements are not reported as success.

Once a JOIN completes WELCOME, Harness mirrors its full local log over the
existing native connection, on the bulk channel. The host writes it to
`KenshiCoop_join_N_mirror.log` separately from its own `KenshiCoop_host.log`;
the join retains `KenshiCoop_join.log`. Forwarding is bounded to eight chunks
of up to 960 bytes per second; pending bytes stay on disk. Save transfer can
delay the bulk channel. Release builds do not forward client logs.

Logs remain on the participants' machines; there is no separate HTTP upload
or outside log collector. With Steam transport, the existing game connection
may use Valve's relay. Logs may contain player names and game data; share them
privately. When distributing an experimental build, provide participants with
its matching source under AGPL-3.0, including any changes from the public tag.


## Credits

- **[nhoral](https://github.com/nhoral) — original author of KenshiCoop:**
  the original architecture, replication, engine hooks and source code.
- [kotetsyy](https://github.com/kotetsyy) — maintenance, changes and releases
  of the derivative KenshiCoop4up project
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) — RE_Kenshi and
  KenshiLib, which make plugins like this possible
- [ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) — native networking
- [zeroit789](https://github.com/zeroit789) — the "Multiplayer (Wanderer x2)"
  co-op game start ([#15](https://github.com/nhoral/KenshiCoop/pull/15))
- Lo-Fi Games — Kenshi

## License

[AGPL-3.0](LICENSE), inherited from the original project. KenshiLib and
RE_Kenshi are GPLv3; this plugin links KenshiLib under GPLv3 section 13
(GPL/AGPL combination). Not affiliated with Lo-Fi Games. Intended as a
non-commercial fan project; this intent adds no restriction to the license rights.
