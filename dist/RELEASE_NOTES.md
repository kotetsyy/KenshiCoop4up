## Join-лог наконец создаётся

Диагностический релиз. В игре ничего не меняется — но со следующей сессии
подключающийся игрок получает свой лог, а без него разбирать рассинхроны
вслепую.

### Почему join-лога не было

Он не «не писался» — его **никогда не открывали под этим именем**. Имя файла
выбиралось при загрузке плагина, задолго до того, как игрок трогает F2: из
ключа `role` в `coop_config.json`, который по умолчанию `"host"`.

То есть `role` — это запомненное намерение, а не роль сессии. Свежая установка,
подключающаяся к другу, писала всю сессию в `KenshiCoop_host.log`, и join-лог,
который у игроков просили, просто не существовал.

### Что теперь

Лог переезжает в файл, соответствующий **фактической** роли, в момент Connect —
там роль уже настоящая.

- Старый файл получает строку `log continues in …`, новый — `log opened
  (continued from …)`. Лог, обрывающийся на моменте Connect, — это то, что
  тратит время при разборе.
- В новый файл перевыпускается стартовый баннер: сборка, `effective cfg`,
  `CAPS`. Иначе присланный join-лог был бы без самых нужных строк — они бы
  остались в файле, который никто не отправляет.
- Только при реальной смене файла, чтобы в host-логе не копились дубликаты
  `role=` и `effective cfg`.

Настраивать ничего не нужно: достаточно этой сборки у каждого.

### Если собираете логи

Роль лучше выбрать **до** первого Connect. Файлы открываются на перезапись, так
что подключение хостом, а затем переключение на join обрежет host-лог.

### Что по-прежнему не проверено

- **Фикс вылета на луте** не проходил живую сессию на двоих.
- **Ник на юните в отряде** не подтверждён в реальной игре вдвоём.
- **Трое и четверо игроков** реализованы, но не проверены.

### Установка

Если обновления включены — апдейтер подтянет сам. Иначе при закрытой игре
положите все три файла в `<Kenshi>\mods\KenshiCoop\`: `KenshiCoop.dll`,
`RE_Kenshi.json` и `KenshiCoop.mod`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

<details>
<summary><b>🇬🇧 Read this in English</b></summary>

## The join log finally gets created

A diagnostics release. Nothing changes in game — but from the next session the
joining player gets their own log, and without one desyncs are argued about
blind.

### Why there was no join log

It was not "failing to write" — it was **never opened under that name**. The
file name was chosen at plugin load, long before anyone touches F2, from the
`role` key in `coop_config.json`, which defaults to `"host"`.

So `role` is a remembered intent, not the role of the session. A fresh install
that joined a friend wrote its entire session into `KenshiCoop_host.log`, and
the join log players kept being asked for simply did not exist.

### What happens now

The log moves to the file matching the **actual** role at Connect, which is
where the role first becomes real.

- The old file gets a `log continues in …` line and the new one `log opened
  (continued from …)`. A log that just stops at the moment of Connect is the
  thing that wastes time when reading it.
- The startup banner is re-emitted into the new file: build stamp, `effective
  cfg`, `CAPS`. Otherwise the join log people send would be missing exactly the
  lines wanted first — they would be stranded in the file nobody sends.
- Only on a real switch, so the host log does not accumulate duplicate `role=`
  and `effective cfg` lines.

Nothing to configure: everyone just needs this build.

### If you are collecting logs

Pick the role **before** the first Connect. Files open truncating, so connecting
as host and then switching to join will cut the host log short.

### Still not tested

- **The loot crash fix** has not been through a live two-player session.
- **The nick on the claimed squad unit** is unconfirmed in a real 2-player game.
- **Three and four players** are implemented but unvalidated.

### Install

With updates enabled the in-game updater fetches this. Otherwise, with the game
closed, put all three files in `<Kenshi>\mods\KenshiCoop\`: `KenshiCoop.dll`,
`RE_Kenshi.json` and `KenshiCoop.mod`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
