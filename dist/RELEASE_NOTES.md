## Бой теперь виден с обеих сторон

Вы били врага на хосте, а на клиенте тот же боец просто стоял. Причина нашлась
в логе целиком.

Драка реплицируется не анимацией, а **причиной**: сосед получает «этот боец
атакует вон того» и его собственный движок разыгрывает замах, шаги и удар. В
приказе едет **рука цели** — идентификатор тела в системе координат отправителя.

Беда в том, что тело, которое клиент создал у себя как копию (`[rekey]`), живёт
под **другой локальной рукой**. В этой сессии враг «Сау» у хоста был
`1,2475097600`, а у клиента — `1,3468139776`. Приход приказа искал цель по руке
хоста, не находил ничего и возвращал `r=1` — и так каждые полторы секунды весь
бой: `localFight=0`, боец стоит.

Сторона отправки этот перевод уже делала (`[combat] CAP xlate`); недоставало
ровно такого же шага на стороне приёма. Теперь рука цели переводится в локальную
один раз, до всего остального.

Заодно чинится второе, менее заметное: проверка «а не дерётся ли копия не с тем»
сравнивала показания локального движка с рукой **с провода**. Для копии они
разные всегда, то есть правильно начатый бой считался ошибочным и сбрасывался
`clearGoals` на каждом переприказе. Теперь обе величины в одной системе координат.

В лог добавлена строка `[combat] APPLY xlate` — видно, когда перевод сработал.

### Что проверено и оказалось в порядке

- **Лишних NPC у клиента нет.** Была версия, что «разные объекты» — это
  разъехавшийся мир. Аудит её не подтверждает: `ghost=0` всю сессию, ни одного
  неучтённого локального тела. Из 199 промахов усыновления 192 — «такого NPC
  здесь просто нет», это штатный минт, а не дубль.
- **Обмен между отрядами работает.** Все четыре передачи: `verdict=accept
  applied=1/1`.
- **Перенос тел не пересобирается вхолостую** — правка 0.1.15 держится.

### Что осталось

Про «разные объекты» я пока не знаю, что именно расходится. В логе видно, как
содержимое трупа на хосте прыгает 4 → 3 → 4 предмета, и как два последних снимка
несут по одному предмету, но с разными хешами — то есть предмет один, а вещь
разная. Дальше без построчного разбора не двинуться: один заход с переменной
`KENSHICOOP_INV_DUMP=1` печатает решение по каждому предмету, и тогда будет видно
причина, а не симптом.

Рагдолл всё ещё может улететь — это отдельная причина, не тронута.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

<details>
<summary>🇬🇧 English</summary>

## Fights now render on both machines

You were swinging at an enemy on the host while the same fighter just stood
there on the client. The log explains it end to end.

A fight is replicated as a **cause**, not as animation: the peer is told "this
fighter is attacking that one" and its own engine plays the draw, the footwork
and the swing. The order carries the **target's hand** — the body's identifier in
the *sender's* key space.

The catch: a body the client minted as a copy (`[rekey]`) lives under a
**different local hand**. This session, the enemy "Сау" was `1,2475097600` on the
host and `1,3468139776` on the client. The arriving order looked the target up by
the host's hand, found nothing, and returned `r=1` — every 1.5 s, all fight long:
`localFight=0`, fighter idle.

The send side already did this translation (`[combat] CAP xlate`); the matching
hop on the receive side was simply missing. The target hand is now translated to
the local one once, before anything else.

That also fixes a quieter second bug: the "is the copy fighting the wrong body"
check compared a local engine read against the **wire** hand. For a copy those
always differ, so a correctly started fight was judged wrong and `clearGoals`-reset
on every re-issue. Both values now live in the same key space.

A `[combat] APPLY xlate` line was added so the translation is visible in the log.

### Checked and clean

- **No extra NPCs on the client.** One theory for "different objects" was a
  diverged world. The audit does not support it: `ghost=0` for the whole session,
  not one unaccounted local body. Of 199 adoption misses, 192 are "no such NPC
  here at all" — a normal mint, not a duplicate.
- **Squad-to-squad transfers work.** All four: `verdict=accept applied=1/1`.
- **Body carry no longer re-issues itself** — the 0.1.15 fix holds.

### Still open

I do not yet know what "different objects" actually refers to. The log shows a
corpse's contents bouncing 4 → 3 → 4 items on the host, and the last two
snapshots each carrying one item but with different hashes — same count,
different thing. Going further needs the per-item breakdown: one session with
`KENSHICOOP_INV_DUMP=1` prints the decision for every item, and then this is a
cause rather than a symptom.

The ragdoll can still fly off — a separate cause, untouched.

### Install

With updates on, the updater fetches it. Otherwise, with the game closed, drop
the three files into `<Kenshi>\mods\KenshiCoop\`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
