# v0.1.0 — panel cleanup, and version numbering restarts here

Fork of **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, whose
author wrote the mod. Protocol **58** — unchanged, so this connects to v0.5x.

> **The version number went "down" on purpose.** This is the same line of work
> that ended at v0.57, renumbered to `MAJOR.MINOR.PATCH` starting at 0.1.0.
> Nothing was rolled back. Read the next section before assuming a mistake.

## Version numbering restarts at 0.1.0

Until now the updater compared build ids for **equality only** — it could not
tell an upgrade from a downgrade, and installed either. A manifest naming an
older build would have pushed every player back onto it, which is fine as a
deliberate rollback and very bad as an accident.

This release orders versions properly: `MAJOR.MINOR.PATCH`, missing components
read as 0, and a build installs **only when it is newer**. A deliberate rollback
is still possible with `allowDowngrade=1` in the manifest.

The renumber had to ship in this exact release. By the new rules `0.57` parses
as 0.57.0 and outranks `0.1.0`, so a client already ordering versions would
refuse to move — but clients on v0.5x still compare by equality, so they take
this build regardless. That window closes now: 0.1.0 → 0.1.1 → 0.2.0 from here,
always forward.

## Panel changes

- **The host no longer has Steam ID paste rows.** They were never needed: the
  host accepts whoever dials in, so it does not have to know an id in advance.
  Three "Friend N" rows made a two-player setup look like a four-step chore.
- **The join no longer shows its own Steam ID.** Nobody needs it. Two masked
  ids side by side — yours and the host's — only invited pasting the wrong one.
  What each side actually needs is what it now shows: the host publishes its id,
  the join pastes one.
- **Your own id is labelled.** It used to render as a bare `****1843` with
  nothing saying what it was.
- **The window is trimmed** from 72% of screen height to 40%. It was sized for
  its worst case and every layout left two thirds empty rust.

A fit-to-content resize was tried first and reverted: `getContentHeight()` does
not return the units `resize()` consumes, so the panel clipped its own toggle
buttons off the bottom edge after a role switch. The reasoning is recorded in
the code so the next attempt does not repeat it.

## Still not tested

- **The loot use-after-free fix from v0.52** has not been through a live
  two-player session. The check: host keeps a corpse window open, the joining
  player takes everything and reopens it — no crash, no loot reappearing — then
  the host closes the window and the corpse is empty for both. The host log
  should show `[inv] GUI-DEFER` while the window is open, then one
  `[inv] GUI-RESUME` after it closes.
- **The nick on the claimed squad unit** is unconfirmed in a real 2-player game.
- **Three and four players** are implemented but unvalidated.

## Install

Already on v0.5x with updates enabled? The in-game updater fetches this.
Otherwise put all three files in `<Kenshi>\mods\KenshiCoop\`, with the game
closed: `KenshiCoop.dll`, `RE_Kenshi.json` (RE_Kenshi will not load the plugin
without it) and `KenshiCoop.mod` (so it appears in the Mods menu). Requires
Kenshi 1.0.65 and [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

# v0.1.0 — чистка панели, и нумерация версий начинается заново (Русский)

Форк **[nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop)**, автор мода —
он. Протокол **58**, не менялся: соединяется с v0.5x.

> **Номер версии понизился намеренно.** Это та же линия работы, что закончилась
> на v0.57, переномерованная в `MAJOR.MINOR.PATCH` начиная с 0.1.0. Ничего не
> откатывали. Прежде чем счесть это ошибкой — прочитайте следующий раздел.

## Нумерация начинается с 0.1.0

До сих пор апдейтер сравнивал версии **только на равенство** — он не отличал
обновление от отката и ставил и то, и другое. Манифест со старой версией вернул
бы всех игроков на неё: как намеренный откат это удобно, как случайность — очень
плохо.

Теперь версии упорядочены: `MAJOR.MINOR.PATCH`, недостающие части читаются как 0,
и сборка ставится **только если она новее**. Намеренный откат по-прежнему
возможен — ключом `allowDowngrade=1` в манифесте.

Переномерация обязана была выйти именно в этом релизе. По новым правилам `0.57`
разбирается как 0.57.0 и старше `0.1.0`, так что клиент, уже сравнивающий по
порядку, отказался бы переходить. Но клиенты на v0.5x пока сравнивают по
равенству и возьмут эту сборку без вопросов. Это окно сейчас закрывается: дальше
0.1.0 → 0.1.1 → 0.2.0, всегда вперёд.

## Изменения в панели

- **У хоста больше нет строк вставки Steam ID.** Они были не нужны: хост
  принимает любого, кто к нему постучится, и знать ID заранее ему не требуется.
  Три строки «Friend N» превращали настройку на двоих в четыре шага.
- **У join больше не показывается свой Steam ID.** Он никому не нужен. Два
  замаскированных номера рядом — свой и хоста — только провоцировали вставить
  не тот. Теперь каждая сторона видит ровно то, что ей нужно: хост свой ID
  отдаёт, join чужой вставляет.
- **Свой ID подписан.** Раньше висело голое `****1843` без пояснений.
- **Окно ужато** с 72% высоты экрана до 40%. Оно было рассчитано на худший
  случай, и в любой раскладке две трети оставались пустой ржавчиной.

Сначала пробовали подгонку под содержимое — откатили: `getContentHeight()`
возвращает не те единицы, которые принимает `resize()`, и после смены роли
панель срезала себе кнопки переключателей. Причина записана в коде, чтобы
следующая попытка не повторила её.

## Что по-прежнему не проверено

- **Фикс вылета на луте из v0.52** не проходил живую сессию на двоих. Проверка:
  хост держит окно трупа открытым, подключившийся забирает всё и открывает
  заново — вылета нет и лут не возрождается, — затем хост закрывает окно, и труп
  пуст у обоих. В логе хоста при открытом окне должны идти `[inv] GUI-DEFER`, а
  после закрытия — одна `[inv] GUI-RESUME`.
- **Ник на юните в отряде** не подтверждён в реальной игре вдвоём.
- **Трое и четверо игроков** реализованы, но не проверены.

## Установка

Уже на v0.5x с включёнными обновлениями? Апдейтер подтянет сам. Иначе положите
при закрытой игре все три файла в `<Kenshi>\mods\KenshiCoop\`: `KenshiCoop.dll`,
`RE_Kenshi.json` (без него RE_Kenshi не загрузит плагин) и `KenshiCoop.mod`
(чтобы мод был в меню Mods). Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).
