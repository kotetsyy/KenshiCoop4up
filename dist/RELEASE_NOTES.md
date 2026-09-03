## Чистка панели, и нумерация версий начинается заново

> **Номер версии понизился намеренно.** Это продолжение той же линии, что
> закончилась на v0.57, переномерованное в `MAJOR.MINOR.PATCH` начиная с 0.1.0.
> Ничего не откатывали.

### Нумерация начинается с 0.1.0

Раньше апдейтер сравнивал версии **только на равенство** — он не отличал
обновление от отката и ставил и то, и другое. Манифест, собранный из старого
рабочего дерева, вернул бы всех игроков на старую сборку.

Теперь версии упорядочены: `MAJOR.MINOR.PATCH`, недостающие части читаются как
0, сборка ставится **только если она новее**. Намеренный откат остался возможен
— ключом `allowDowngrade=1` в манифесте.

Переномерация обязана была выйти именно сейчас: по новым правилам `0.57`
разбирается как 0.57.0 и старше `0.1.0`, так что клиент, уже сравнивающий по
порядку, отказался бы переходить. Клиенты на v0.5x пока сравнивают по равенству
и возьмут эту сборку сами. Дальше 0.1.0 → 0.1.1 → 0.2.0, только вперёд.

### Изменения в панели

- **У хоста больше нет строк вставки Steam ID.** Они были не нужны: хост
  принимает любого, кто к нему постучится, и знать ID заранее ему не требуется.
  Три строки «Friend N» превращали настройку на двоих в четыре шага.
- **У join больше не показывается свой Steam ID.** Он никому не нужен. Два
  замаскированных номера рядом — свой и хоста — только провоцировали вставить
  не тот. Теперь каждая сторона видит ровно тот идентификатор, с которым
  действует: хост свой отдаёт, join чужой вставляет.
- **Свой ID подписан.** Раньше висело голое `****1843` без пояснений.
- **Окно ужато** с 72% высоты экрана до 40%: оно было рассчитано на худший
  случай, и в любой раскладке две трети оставались пустыми.

Сначала пробовали подгонку под содержимое — откатили: `getContentHeight()`
возвращает не те единицы, которые принимает `resize()`, и после смены роли
панель срезала себе кнопки переключателей.

### Что по-прежнему не проверено

- **Фикс вылета на луте** не проходил живую сессию на двоих. Проверка: хост
  держит окно трупа открытым, подключившийся забирает всё и открывает заново —
  вылета нет и лут не возрождается, — затем хост закрывает окно, и труп пуст у
  обоих. В логе при открытом окне идут `[inv] GUI-DEFER`, после закрытия — одна
  `[inv] GUI-RESUME`.
- **Ник на юните в отряде** не подтверждён в реальной игре вдвоём.
- **Трое и четверо игроков** реализованы, но не проверены.

### Установка

Если у вас v0.5x с включёнными обновлениями — апдейтер подтянет сам. Иначе при
закрытой игре положите все три файла в `<Kenshi>\mods\KenshiCoop\`:
`KenshiCoop.dll`, `RE_Kenshi.json` (без него RE_Kenshi не загрузит плагин) и
`KenshiCoop.mod` (чтобы мод был в меню Mods). Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

<details>
<summary><b>🇬🇧 Read this in English</b></summary>

> **The version number went down on purpose.** This continues the line that
> ended at v0.57, renumbered to `MAJOR.MINOR.PATCH` starting at 0.1.0. Nothing
> was rolled back.

#### Version numbering restarts at 0.1.0

The updater used to compare versions by **equality only** — it could not tell an
upgrade from a downgrade and installed either. A manifest regenerated from a
stale checkout would have pulled every player back onto an old build.

Versions are now ordered: `MAJOR.MINOR.PATCH`, missing components read as 0, and
a build installs **only when it is newer**. A deliberate rollback is still
possible with `allowDowngrade=1` in the manifest.

The renumber had to ship now: by the new rules `0.57` parses as 0.57.0 and
outranks `0.1.0`, so a client already ordering versions would refuse to move.
Clients on v0.5x still compare by equality and take this build on their own.
From here it is 0.1.0 → 0.1.1 → 0.2.0, always forward.

#### Panel changes

- **The host no longer has Steam ID paste rows.** They were never needed: the
  host accepts whoever dials in and does not have to know an id in advance.
  Three "Friend N" rows made a two-player setup look like a four-step chore.
- **The join no longer shows its own Steam ID.** Nobody needs it, and two masked
  ids side by side invited pasting the wrong one. Each role now shows exactly
  the id it acts on: the host publishes one, the join pastes one.
- **Your own id is labelled.** It used to render as a bare `****1843`.
- **The window is trimmed** from 72% of screen height to 40%; it was sized for
  its worst case and every layout left two thirds empty.

A fit-to-content resize was tried first and reverted: `getContentHeight()` does
not return the units `resize()` consumes, and the panel clipped its own toggle
buttons off the bottom edge after a role switch.

#### Still not tested

- **The loot crash fix** has not been through a live two-player session. The
  check: host keeps a corpse window open, the joining player takes everything
  and reopens it — no crash, no loot reappearing — then the host closes the
  window and the corpse is empty for both. The log shows `[inv] GUI-DEFER` while
  the window is open, then one `[inv] GUI-RESUME` after it closes.
- **The nick on the claimed squad unit** is unconfirmed in a real 2-player game.
- **Three and four players** are implemented but unvalidated.

#### Install

On v0.5x with updates enabled? The in-game updater fetches this. Otherwise, with
the game closed, put all three files in `<Kenshi>\mods\KenshiCoop\`:
`KenshiCoop.dll`, `RE_Kenshi.json` (RE_Kenshi will not load the plugin without
it) and `KenshiCoop.mod` (so it appears in the Mods menu). Requires Kenshi
1.0.65 and [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
