# v0.52 — first release of this fork

Fork of **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**. The mod
is nhoral's work; this release carries three changes on top of it.

Protocol **58**. **Everyone must be on the same build** — the protocol version is
checked at handshake and mismatched versions refuse to connect. This is the
first build that can update itself, so it is the last one you have to hand out
by hand.

---

## 1. Fixed: crash while looting with an inventory window open

**Symptom.** Host opens a corpse, the joining player loots it, and the game
dies. Sometimes the loot came back on reopen first.

**Cause.** Applying a peer's inventory snapshot destroys the items the peer no
longer has. Kenshi hands every icon in an open inventory panel a raw pointer to
its item and never revalidates them, so destroying a stack under a live window
leaves the panel pointing at freed memory — and the next repaint dereferences it
**on the render thread**, where the mod's own exception handlers are not on the
stack.

The host log for the reported crash shows exactly this: three first-chance reads
of `0xFFFFFFFFFFFFFFFF` inside the mod (caught and swallowed), then 85 ms later a
fatal read of the same address at `kenshi_x64.exe+0x74898f` on a *different*
thread. The previous mitigation — pointer sanity checks plus SEH — could never
have worked: it only ever guarded the mod's side of the fault.

**Fix.** Two layers:

- Applying a snapshot is **deferred entirely** while a panel is open on that
  container, and retried each tick. Closing the window applies the newest state
  immediately. The whole snapshot waits rather than half of it, because creating
  now and destroying later would leave a divergence no later snapshot describes.
- The reconcile itself **refuses to destroy anything** under an open panel
  regardless of caller, degrading to additive-only.

Detection uses `Inventory::getInventoryGUI`, reported as `inv_gui` in the
`[engine] CAPS` log line. It also covers a carried container's own window, which
is the backpack case.

**Known limitation, unchanged:** while the host keeps a corpse window open, that
window may keep showing items the join already took. Closing it settles both
sides. Ghosts in a stale panel are the accepted trade for not corrupting memory.

## 2. New: the mod updates itself

Everyone having to run the same DLL is a real problem — a single stale copy just
reads as "it will not connect", with nothing explaining why.

At startup a background thread (the game never waits on it) fetches a small
manifest over HTTPS, compares it against the version built into the DLL, and if
a newer build exists downloads it, verifies its SHA-256, and swaps it in.
Windows allows renaming a mapped image even though it forbids overwriting one,
which is what lets the DLL replace itself. **The running session keeps the old
code; the update takes effect on your next launch**, and the F2 panel says so
rather than pretending it is already live.

Transport is pinned to GitHub over HTTPS, automatic redirects are disabled (the
download URL inside the manifest is treated as data, and its host is re-checked
after a redirect), the payload must be a PE image, and the SHA-256 must match or
it is discarded.

Configurable in `coop_config.json` next to the DLL — `updateEnabled`,
`updateAutoApply` (report only, download nothing), `updateOwner`, `updateRepo`.
Note that whoever controls the configured repository can put code on your
machine; the hash check verifies the channel, not the intent.

## 3. Rolled up: the accumulated protocol 55 → 58 work

This was sitting uncommitted on the tree. It is nhoral's work, listed here so
the release is not silent about what it contains:

- Knocked-out and dead bodies are always streamed and latched, so a body that is
  down on the host no longer stands up on the join.
- `SpawnInfo.dead=2` describes a knocked-out spawn, and spawn replies carry the
  knockout event.
- NPC spawn budget cut to one body per tick — NPCs used to appear one at a time
  and then three or four at once.
- Thrown bodies fly host-authoritative with a landing settle, so a corpse lands
  in the same place for both players.
- Drop pose travels on the wire.
- Game speed is last-write-wins with pause independent of the multiplier, fixing
  speed sticking at 5x with no way back to 1x or 2x.
- The knockout timer no longer oscillates in the medical UI.
- Steam IDs and the UDP endpoint are remembered in `coop_config.json`, so a
  relaunch pre-fills the F2 panel instead of asking for the clipboard again.

---

## Install

Put `KenshiCoop.dll` at `<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` and enable
**KenshiCoop** in the Mods menu. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847). Close the game before
replacing the file — a running Kenshi holds the DLL locked. Every player needs
this same build; after that the updater handles it.

## Not tested

Stated plainly so nobody is surprised:

- **The loot fix has not been through a live two-player session.** The intended
  check: host keeps a corpse window open, join takes everything, join reopens —
  no crash, no loot reappearing — then the host closes the window and the corpse
  is empty for both.
- **The updater has never run against a real release.** This build is the first
  one published, so nothing has had an update to find yet.
- **Three and four players** are implemented but unvalidated.

---

# v0.52 — первый релиз этого форка (Русский)

Форк **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**. Мод —
работа nhoral; этот релиз несёт поверх неё три изменения.

Протокол **58**. **У всех должна быть одна и та же сборка** — версия протокола
проверяется при рукопожатии, разные версии соединяться отказываются. Это первая
сборка, умеющая обновлять себя сама, то есть последняя, которую надо раздавать
руками.

---

## 1. Исправлено: вылет при луте с открытым окном инвентаря

**Симптом.** Хост открывает труп, подключившийся игрок его обирает — игра
падает. Иногда перед этим лут возвращался при повторном открытии.

**Причина.** Применение снапшота инвентаря удаляет предметы, которых у соседа
больше нет. Kenshi выдаёт каждой иконке в открытой панели инвентаря сырой
указатель на предмет и никогда их не перепроверяет. Поэтому удаление стака под
живым окном оставляет панель со ссылкой на освобождённую память, и следующая
перерисовка разыменовывает её **на рендер-потоке**, где обработчиков исключений
мода на стеке нет.

Лог хоста показывает ровно это: три чтения `0xFFFFFFFFFFFFFFFF` внутри мода
(пойманы и проглочены), а через 85 мс — фатальное чтение того же адреса в
`kenshi_x64.exe+0x74898f` на **другом** потоке. Прежняя мера — проверки
указателей плюс SEH — не могла помочь в принципе: она прикрывала только сторону
мода.

**Что сделано.** Два уровня:

- Применение снапшота **полностью откладывается**, пока на контейнере открыта
  панель, и повторяется каждый тик. Закрытие окна применяет свежее состояние
  сразу. Ждёт весь снапшот, а не половина: создать сейчас и удалить потом —
  значит получить расхождение, которое ни один следующий снапшот не описывает.
- Сам reconcile **отказывается что-либо удалять** под открытой панелью,
  независимо от вызывающего, и вырождается в «только добавлять».

Определяется через `Inventory::getInventoryGUI`, в логе `[engine] CAPS` это
`inv_gui`. Покрывает и окно носимого контейнера — то есть случай с рюкзаком.

**Известное ограничение, оно осталось:** пока хост держит окно трупа открытым,
это окно может показывать вещи, которые подключившийся уже забрал. Закрытие окна
приводит обе стороны в порядок. Призраки в устаревшей панели — осознанная плата
за то, чтобы не портить память.

## 2. Новое: мод обновляет себя сам

Требование «у всех одинаковая DLL» — реальная проблема: одна устаревшая копия
выглядит просто как «не коннектится», без объяснений.

При старте фоновый поток (игра его не ждёт) забирает по HTTPS небольшой
манифест, сравнивает с версией, вшитой в DLL, и если есть более новая сборка —
скачивает её, проверяет SHA-256 и подменяет файл. Windows разрешает
переименовать замапленный образ, хотя перезаписать не даёт, — на этом и держится
самоподмена. **Текущая сессия продолжает работать на старом коде, обновление
вступает в силу со следующего запуска**, и панель F2 об этом честно пишет, а не
делает вид, что уже применилось.

Канал закреплён за GitHub по HTTPS, автоматические редиректы отключены (ссылка
на загрузку внутри манифеста считается данными, и после редиректа хост
проверяется заново), содержимое обязано быть PE-образом, а SHA-256 — совпасть,
иначе файл выбрасывается.

Настраивается в `coop_config.json` рядом с DLL: `updateEnabled`,
`updateAutoApply` (только сообщать, ничего не качать), `updateOwner`,
`updateRepo`. Учтите: владелец указанного репозитория может доставить код на
вашу машину; проверка хеша подтверждает канал, а не намерения.

## 3. Собрано в кучу: накопленная работа по протоколу 55 → 58

Она лежала в дереве незакоммиченной. Это работа nhoral, перечислена, чтобы
релиз не умалчивал о своём содержимом:

- Тела без сознания и трупы всегда стримятся и защёлкиваются — тело, лежащее у
  хоста, больше не стоит у подключившегося.
- `SpawnInfo.dead=2` описывает спавн в нокауте, ответ на спавн несёт событие
  нокаута.
- Бюджет спавна NPC срезан до одного тела за тик — раньше NPC появлялись по
  одному, а потом сразу по трое-четверо.
- Брошенные тела летят под управлением хоста с посадкой в конце, поэтому труп
  ложится в одно и то же место у обоих.
- Поза при броске передаётся по сети.
- Скорость игры работает по принципу «последний клик побеждает», пауза
  независима от множителя — починено залипание на 5x без возврата к 1x/2x.
- Таймер нокаута больше не скачет в медицинском интерфейсе.
- Steam ID и UDP-адрес запоминаются в `coop_config.json` — при перезапуске
  панель F2 заполнена, буфер обмена больше не нужен.

---

## Установка

Положите `KenshiCoop.dll` в `<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` и включите
**KenshiCoop** в меню модов. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847). Перед заменой файла
закройте игру — запущенный Kenshi держит DLL заблокированной. Эта сборка нужна
каждому игроку; дальше справится автообновление.

## Что не протестировано

Говорю прямо, чтобы не было сюрпризов:

- **Фикс лута не прошёл живую сессию на двоих.** Проверять так: хост держит окно
  трупа открытым, подключившийся забирает всё и открывает заново — вылета нет и
  лут не возрождается, — затем хост закрывает окно, и труп пуст у обоих.
- **Автообновление ни разу не работало с настоящим релизом.** Эта сборка
  публикуется первой, находить ей пока нечего.
- **Три и четыре игрока** реализованы, но не валидированы.
