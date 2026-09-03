# v0.54 — F2 nick on the squad unit you play

Fork of **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, whose
author wrote the mod.

Protocol **58** — unchanged, so v0.53 and v0.54 still connect to each other.
Everyone still needs the same DLL: this is a feature drop, not a wire break.

## What changed

**Paste a display nick in the F2 Co-op Session panel.** The panel still has no
text field, so it works like Steam ID: copy a name, click **Nick**, and the
row shows what it captured. It is remembered in `coop_config.json`.

When you join (or host), that nick is sent in the existing HELLO / WELCOME
name field and written onto the squad unit you own, so the nametag tells you
who to play. Paste it **before** going ONLINE so the handshake carries it.

```
Nick: Alice    (click to re-paste)
```

## Still not tested

- **The nick itself has not been through a live two-player session.** Check:
  both players paste a nick, go online, and the claimed unit's nametag matches.
  The log should show `[coop-ui] paste nick` then `[nick] applied id=…`.
- **The loot use-after-free fix from v0.52** is still unconfirmed in 2p.
- **Three and four players** are implemented but unvalidated.

## Install

If you are already on v0.53 with updates enabled, the in-game updater will
fetch this. Otherwise put `KenshiCoop.dll` at
`<Kenshi>\mods\KenshiCoop\KenshiCoop.dll`, with the game closed. Requires
Kenshi 1.0.65 and [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

# v0.54 — ник из F2 на юните в отряде (Русский)

Форк **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, автор мода —
он.

Протокол **58**, не менялся: v0.53 и v0.54 по-прежнему соединяются. DLL всё
равно нужна одна и та же — это фича, не смена провода.

## Что изменилось

**В панели F2 можно задать ник.** Поля ввода нет, поэтому как со Steam ID:
скопируйте имя, нажмите **Nick**, панель покажет, что вставила. Запоминается
в `coop_config.json`.

При входе на сервер ник уходит в уже существующее поле имени в HELLO / WELCOME
и ставится на юнит в вашем отряде — по табличке видно, за кого играть. Вставьте
ник **до** ONLINE, чтобы он попал в рукопожатие.

```
Nick: Alice    (click to re-paste)
```

## Что по-прежнему не проверено

- **Сам ник не прошёл живую сессию на двоих.** Проверка: оба вставили ник,
  зашли, табличка на своём юните совпадает. В логе: `[coop-ui] paste nick`,
  потом `[nick] applied id=…`.
- **Фикс вылета на луте из v0.52** всё ещё без живой проверки на двоих.
- **Трое и четверо игроков** реализованы, но не проверены.

## Установка

Если вы уже на v0.53 с обновлениями, апдейтер подтянет сам. Иначе положите
`KenshiCoop.dll` в `<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` при закрытой игре.
Нужны Kenshi 1.0.65 и [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

# v0.53 — build version visible in the co-op panel

Fork of **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, whose
author wrote the mod. Small release on top of v0.52.

Protocol **58** — unchanged, so v0.52 and v0.53 still connect to each other.

## What changed

**The F2 panel title now shows which build you are running:**

```
Co-op Session   v0.53 - proto 58    -    F2 to close
```

Protocol sits next to the release id because it is the value that actually
gates a connection: `v0.53` tells two players their builds differ, `proto 58`
tells them whether that is why the handshake refused. It lives in the title
rather than taking one of the panel's few visible rows, since it never changes
during a session.

## Also: the updater's check path is now proven in a real session

v0.52 shipped the self-updater untested against a live release. A real host run
has since logged:

```
[update] checking kotetsyy/KenshiCoop4up ...
[update] up to date (0.52, proto 58)
```

303 ms end to end. So the HTTPS fetch, manifest parse and version comparison are
confirmed working in-game.

**This release is the first test of the remaining half** — download, SHA-256
verification, and the on-disk swap. If it worked for you, you are reading a
panel that says `v0.53` and you did not copy any file by hand.

Remember the shape of it: the update installs on one launch and takes effect on
the **next** one. The session that downloads it keeps running the old code, and
the panel says a restart is needed rather than pretending otherwise.

## Still not tested

- **The loot use-after-free fix from v0.52 has not been through a live
  two-player session.** The check: host keeps a corpse window open, the joining
  player takes everything and reopens it — no crash, no loot reappearing — then
  the host closes the window and the corpse is empty for both. The host log
  should show `[inv] GUI-DEFER` while the window is open and a single
  `[inv] GUI-RESUME` after it closes.
- **Three and four players** are implemented but unvalidated.

## Install

Only needed if you are not on v0.52 already — otherwise the updater handles it.
Put `KenshiCoop.dll` at `<Kenshi>\mods\KenshiCoop\KenshiCoop.dll`, with the game
closed. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

# v0.53 — версия сборки видна в кооп-панели (Русский)

Форк **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, автор мода —
он. Небольшой релиз поверх v0.52.

Протокол **58**, не менялся: v0.52 и v0.53 по-прежнему соединяются друг с другом.

## Что изменилось

**В заголовке панели F2 теперь видно, какая сборка запущена:**

```
Co-op Session   v0.53 - proto 58    -    F2 to close
```

Протокол стоит рядом с версией не для красоты: именно он гейтит соединение.
`v0.53` скажет двум игрокам, что сборки разные, а `proto 58` — является ли это
причиной отказа при рукопожатии. Размещено в заголовке, а не отдельной строкой,
потому что за сессию значение не меняется, а видимых строк в панели мало.

## Заодно: проверка обновлений подтверждена в реальной сессии

v0.52 уезжал с апдейтером, ни разу не работавшим с настоящим релизом. С тех пор
реальный запуск хоста записал в лог:

```
[update] checking kotetsyy/KenshiCoop4up ...
[update] up to date (0.52, proto 58)
```

303 мс на весь цикл. То есть HTTPS-запрос, разбор манифеста и сравнение версий
в игре работают.

**Этот релиз — первая проверка второй половины:** скачивания, сверки SHA-256 и
подмены файла на диске. Если сработало, вы читаете панель с надписью `v0.53`, и
никаких файлов руками не копировали.

Помните про порядок: обновление устанавливается за один запуск, а вступает в
силу со **следующего**. Сессия, которая его скачала, продолжает работать на
старом коде, и панель честно просит перезапустить, а не делает вид, что уже всё.

## Что по-прежнему не проверено

- **Фикс вылета на луте из v0.52 не прошёл живую сессию на двоих.** Проверка:
  хост держит окно трупа открытым, подключившийся забирает всё и открывает
  заново — вылета нет и лут не возрождается, — затем хост закрывает окно, и труп
  пуст у обоих. В логе хоста при открытом окне должны идти `[inv] GUI-DEFER`, а
  после закрытия — одна `[inv] GUI-RESUME`.
- **Три и четыре игрока** реализованы, но не валидированы.

## Установка

Нужна только если у вас не v0.52 — иначе справится автообновление. Положите
`KenshiCoop.dll` в `<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` при закрытой игре.
Нужны Kenshi 1.0.65 и [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).
