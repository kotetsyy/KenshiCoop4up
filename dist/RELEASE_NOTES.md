## Вещь, надетая на труп, больше не дублируется

Хост надевает вещь на труп — она надевается, и рядом с трупом у клиента лежит
вторая такая же. Причина в логе, и автор дюпа сам её признаёт одной строкой ниже.

За сохранностью снаряжения следит отдельный канал: он сравнивает содержимое
своего отряда тик к тику, и **уменьшение** считает выброшенной на землю вещью —
чтобы сосед перенёс свою копию туда же. Проверок было три: это не обмен между
отрядами, это не незавершённая передача, и рядом действительно лежит свободная
вещь на земле.

Ни одна из них не срабатывает, когда вещь кладут **в другой инвентарь**. Труп —
не отряд соседа, значит не обмен. На земле ничего нет, значит проверка «лежит
рядом» промахивается — а она и должна промахиваться, для честно выброшенного в
городе оружия движок тоже ничего не находит, ради этого запасной путь и писался.
Дебаунс истекает, и уходит пакет «выброшено».

Дальше у соседа два канала независимо делают своё: снимок инвентаря надевает вещь
на труп, а пакет «выброшено» кладёт рядом с трупом вторую копию — а если своей
копии не нашлось, то и вовсе **создаёт новую** (`APPLY-HEALED ... rebuilt from
intent provenance`). Отсюда и вещь на трупе, и вещь на земле.

Свой же вердикт хост выносит через десять секунд:

```
[wd] ground-prune sid='2309-clothes_v1.mod' drop=0/4 (2044 consecutive reads over 10002ms, everLive=0: not a free ground item)
```

`everLive=0` — «за две тысячи чтений ни разу не прочиталось как свободный предмет
на земле». То есть выброса не было вовсе, и это было известно — просто поздно.

Теперь перед публикацией выброса проверяется сам предмет: если он **жив и всё ещё
лежит внутри контейнера**, выброса не было, и пакет не уходит. Вещь реплицируется
одним каналом — снимком инвентаря. Подавляет только *положительное* чтение: если
указатель мёртв или нечитаем, поведение прежнее, потому что «движок выгрузил
объект» — это ровно тот случай, ради которого запасной путь и существует.

В лог добавлена строка `[wd] decrease-moved`.

### Бой из 0.1.16 подтверждён

В вашей сессии на стороне клиента **60 приказов из 60 — `r=2`**, цель нашлась
каждый раз, и в 34 из них `localFight=1` — копия реально дерётся. До правки было
`r=1` подряд и `localFight=0` весь бой.

### Что осталось и почему я это не трогал

Разная обстановка на хосте и клиенте — **не баг репликации, а разошедшийся мир**.
Костры кочевых лагерей из `nodes_otto1.mod` стоят у вас и у друга на 14–84 юнита
друг от друга; порог сопоставления — 5. Эти лагеря движок расставляет сам при
загрузке, каждая машина своим броском, и сейв тут ни при чём. Канал `[fixture]`
эту разницу измеряет, а не создаёт: расширение порога сведёт объекты в коде, но
не сдвинет их на экране. Настоящее решение — транслировать раскладку лагеря
целиком, и это отдельная большая работа, а не правка числа.

Рагдолл всё ещё может улететь. Не тронуто.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

<details>
<summary>🇬🇧 English</summary>

## Clothing put on a corpse no longer duplicates

The host dresses a corpse — the item goes on, and a second copy of it lies on the
ground beside the body on the client. The log explains it, and the code that
authors the duplicate admits to it one line later.

Gear conservation has its own channel: it compares an owned squad's contents tick
to tick and reads a **decrease** as an item dropped on the ground, so the peer can
relocate its copy to the same spot. It had three guards: this is not a
squad-to-squad trade, not an in-flight transfer, and there really is a free item
lying on the ground nearby.

None of them fires when the item is put **into another inventory**. A corpse is
not the peer's squad, so it is not a trade. Nothing is on the ground, so the "is
it lying nearby" query misses — and it is *supposed* to miss sometimes: for a
genuinely dropped weapon in a town the engine finds nothing either, which is why
the fallback path was written. The debounce expires and a "dropped" packet goes
out.

On the peer, two channels then act independently: the inventory snapshot puts the
item on the corpse, and the drop packet places a second copy beside it — or, if no
local copy is found, **manufactures a new one** (`APPLY-HEALED ... rebuilt from
intent provenance`). Hence one worn and one on the ground.

The host reaches its own verdict ten seconds later:

```
[wd] ground-prune sid='2309-clothes_v1.mod' drop=0/4 (2044 consecutive reads over 10002ms, everLive=0: not a free ground item)
```

`everLive=0` — "over two thousand reads it never once read as a free ground
item". There was no drop at all, and that was knowable — just too late.

Now, before a drop is published, the object itself is checked: if it is **alive
and still inside a container**, no drop happened and no packet goes out. The item
replicates through one channel, the inventory snapshot. Only a *positive* read
suppresses: a dead or unreadable handle keeps the old behaviour, because "the
engine streamed the object out" is exactly the case the fallback exists for.

New log line: `[wd] decrease-moved`.

### The 0.1.16 combat fix is confirmed

In your session, on the client, **60 of 60 orders returned `r=2`** — the target
resolved every time — and 34 of them show `localFight=1`, the copy actually
fighting. Before the fix it was `r=1` throughout and `localFight=0` all fight.

### Still open, and why I left it alone

The different scenery on host and client is **not a replication bug — the two
worlds genuinely differ**. Nomad camp fires from `nodes_otto1.mod` stand 14–84
units apart on the two machines; the matching threshold is 5. The engine lays
those camps out itself at load time, each machine with its own roll, and the save
has nothing to do with it. The `[fixture]` channel measures that difference, it
does not cause it: widening the threshold would pair the objects in code without
moving them on screen. The real fix is to stream the camp layout itself, which is
separate, substantial work — not a changed number.

The ragdoll can still fly off. Untouched.

### Install

With updates on, the updater fetches it. Otherwise, with the game closed, drop
the three files into `<Kenshi>\mods\KenshiCoop\`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
