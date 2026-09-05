## Пустой торговец и разные NPC — это одна причина

Трактирщица у хоста: к.8527 и полный прилавок. У клиента она же: к.0 и пусто.
Разгадка нашлась целиком, и она же объясняет, почему у вас разное население.

Смотрите на время в логе клиента:

```
01:32:40.851  [load] JOIN load suppression ON      <- загрузка закончилась
01:32:41.101  [spawn] proxy ADOPT ... (4 штуки)
01:32:41.101  [spawn] adopt MISS  ... (20 штук, все "no same-template body in reach")
```

**250 миллисекунд.** Первая перепись мира приходит через четверть секунды после
загрузки — а Kenshi заселяет город постепенно, в этой сессии заселение шло
секунд тринадцать. То есть в момент переписи горожан на клиенте ещё нет. Мод не
находит, кого усыновить, и **создаёт двадцать тел заново по шаблону**.

Дальше всё предрешено. Созданные тела остаются навсегда, а когда настоящие
горожане наконец появляются, они оказываются лишними — и их прячут (`hid=14
supp=14`). Город у клиента после этого состоит в основном из свежесозданных
копий.

А созданное по шаблону тело рождается **пустым**: ни товара, ни денег. Вот вам и
к.0 у трактирщицы. Это не потеря товара — его там никогда и не было.

### Правка

В коде уже была ровно та защита, которой не хватало, — но только для дальних тел.
Там прямо написано: «зона рапортует о загрузке за несколько секунд до появления
тел», и дальний минт поэтому ждёт десять секунд непрерывного отсутствия. Ближний
минт не ждал ничего и создавал тело с первого взгляда.

Теперь ждёт так же. Усыновление всё это время продолжает работать, поэтому обычный
исход — не поздний минт, а **отсутствие минта**: настоящий горожанин появляется и
усыновляется, со своим товаром, деньгами и историей.

Цена, честно: по-настоящему новый NPC рядом, которого у клиента правда нет,
теперь появится на десять секунд позже, а не сразу.

В лог добавлена строка `[spawn] INFO deferred (settling)`.

### 0.1.18 подтверждён

`[inv] CENSUS-MUTE` сработал девять раз — столько пустых снимков не ушло к
соседу. Владение в этой сессии было здоровое: `cells=1`, у хоста `mine=22`, у
клиента `mine=0` — то есть мир целиком у хоста, как и задумано.

### Что осталось

Отдельно от всего этого мод **не передаёт инвентарь живого торговца вообще**.
Перепись хоста берёт склады, станки и трупы; живой лавочник не попадает ни в одну
из этих трёх корзин. Пока обе стороны видят одного и того же настоящего торговца,
это незаметно — сейв-то общий. Правка выше именно к этому и ведёт. Если после неё
расхождение у прилавка останется, значит нужен отдельный канал для лавок, и я его
сделаю.

Рагдолл всё ещё может улететь. Не тронуто.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

<details>
<summary>🇬🇧 English</summary>

## The empty trader and the different NPCs are one cause

The barmaid on the host: 8,527 cats and a full counter. The same barmaid on the
client: 0 cats and nothing. The whole chain is in the log, and it also explains
why your populations differ.

Look at the timing on the client:

```
01:32:40.851  [load] JOIN load suppression ON      <- load finished
01:32:41.101  [spawn] proxy ADOPT ... (4 of them)
01:32:41.101  [spawn] adopt MISS  ... (20 of them, all "no same-template body in reach")
```

**250 milliseconds.** The first world census arrives a quarter of a second after
the load — and Kenshi populates a town gradually; in this session it took about
thirteen seconds. So at census time the townsfolk do not exist on the client yet.
The mod finds nobody to adopt and **creates twenty bodies from templates**.

Everything after that follows. Those created bodies are permanent, so when the
real townspeople finally arrive they are surplus and get hidden (`hid=14
supp=14`). The client's town then consists largely of freshly minted copies.

And a body minted from a template is born **empty**: no stock, no money. That is
the barmaid's 0 cats. Nothing was lost — it was never there.

### The fix

The guard that was missing already existed in the code, but only for distant
bodies. It says so outright: "the zone-loaded signal precedes baked-body
materialization by a few seconds", and a far mint therefore waits ten seconds of
continuous absence. A near mint waited for nothing and created a body on first
sight.

Now it waits the same. Adoption keeps running throughout, so the usual outcome is
not a late mint but **no mint**: the real townsperson materializes and is adopted,
with its stock, money and history intact.

The cost, plainly: a genuinely new nearby NPC the client really does lack now
appears ten seconds late instead of at once.

New log line: `[spawn] INFO deferred (settling)`.

### 0.1.18 confirmed

`[inv] CENSUS-MUTE` fired nine times — nine empty snapshots that did not go out.
Authority was healthy this session: `cells=1`, host `mine=22`, join `mine=0` — the
whole world with the host, as intended.

### Still open

Separately from all this, the mod **does not replicate a living trader's inventory
at all**. The host's census covers stores, machines and corpses; a living
shopkeeper falls into none of the three. While both sides see the same real
trader this goes unnoticed — the save is shared. The fix above is what gets you
there. If a discrepancy at the counter survives it, then a dedicated shop channel
is needed and I will write one.

The ragdoll can still fly off. Untouched.

### Install

With updates on, the updater fetches it. Otherwise, with the game closed, drop the
three files into `<Kenshi>\mods\KenshiCoop\`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
