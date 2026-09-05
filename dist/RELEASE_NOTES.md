## Ники третьего игрока

> **Протокол меняется: 58 → 59.** Со старой сборкой соединения не будет.
> Обновиться должны **все**. При включённых обновлениях это произойдёт само.

Втроём каждый клиент видел только два ника — хоста и свой. Третий игрок стоял
безымянным. В логах видно ровно это:

```
хост:      [nick] applied id=0 'kotetsy'
           [nick] applied id=1 'qweqweq'
           [nick] applied id=2 'matiga'

клиент 1:  [nick] applied id=1 'qweqweq'      <- и всё
клиент 2:  [nick] applied id=2 'matiga'       <- и всё
```

Причина простая до обидного. Ник едет в двух пакетах: при подключении клиент
сообщает своё имя хосту, а хост в ответ сообщает своё. **Для двоих это весь
список.** Для троих — нет: клиент 1 и клиент 2 между собой не обмениваются ничем,
и о существовании чужого имени ни один из них не узнаёт никогда.

Всё остальное про третьего игрока при этом уже доходит — я проверил по логам:
клиент 1 ведёт отряд игрока 2, применяет его инвентарь (`items=12`), получает его
характеристики (29 пакетов) и события. Не хватало **только имени**.

Теперь хост рассылает всем таблицу имён целиком, при изменении и раз в десять
секунд на подстраховку. Подключившийся позже сразу узнаёт тех, кто уже играет.
Одна строка на всю таблицу, а не по строке на игрока: список крошечный, а половина
таблицы — это состояние, которого лучше не бывает.

В лог добавлены строки `[nick] roster` (хост) и `roster id=` (клиент).

### Почему пришлось менять протокол

Подходящего канала не было: ни один пакет не возит сведения об игроках всем
сразу. Правило в проекте — не заводить новые типы пакетов, пока не доказано, что
без них никак. Здесь доказано логом: место, куда клиент кладёт чужое имя, у
третьего игрока пустует всю сессию.

Заодно `prototest` вырос с 562 до 567 проверок — новый пакет закрыт тестами на
номер, на отсутствие коллизии и на разбор имени предельной длины.

### Что осталось

Живой торговец по-прежнему не реплицируется как таковой, рагдолл всё ещё может
улететь. Не тронуто.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

<details>
<summary>🇬🇧 English</summary>

## The third player's name

> **Protocol changes: 58 -> 59.** An older build will not connect. **Everyone**
> must update. With updates on this happens by itself.

With three players, each client saw only two names — the host's and its own. The
third player stood there unnamed. The logs show exactly that:

```
host:      [nick] applied id=0 'kotetsy'
           [nick] applied id=1 'qweqweq'
           [nick] applied id=2 'matiga'

client 1:  [nick] applied id=1 'qweqweq'      <- that's all
client 2:  [nick] applied id=2 'matiga'       <- that's all
```

The cause is almost embarrassingly simple. A name travels in two packets: on
connect a client tells the host its name, and the host replies with its own.
**For two players that is the entire roster.** For three it is not: client 1 and
client 2 exchange nothing with each other, so neither ever learns the other's
name.

Everything else about the third player already arrives — I checked it in the logs:
client 1 drives player 2's squad, applies their inventory (`items=12`), receives
their stats (29 packets) and their events. Only the name was missing.

The host now broadcasts the whole name table, on change and every ten seconds as a
backstop, so a player who connects later immediately learns who is already
playing. One row for the entire table rather than a row per player: the list is
tiny, and half a table is the worst state to be in.

New log lines: `[nick] roster` (host) and `roster id=` (client).

### Why the protocol had to change

There was no suitable channel: no existing packet carries per-player information
to everyone. The project rule is not to add packet types until it is proven
necessary. The log proves it here — the slot where a client stores another
player's name sits empty for the whole session on the third player.

`prototest` also grew from 562 to 567 checks: the new packet is covered for its
tag, for tag collisions, and for parsing a maximum-length name.

### Still open

A living trader is still not replicated as such, and the ragdoll can still fly
off. Untouched.

### Install

With updates on, the updater fetches it. Otherwise, with the game closed, drop the
three files into `<Kenshi>\mods\KenshiCoop\`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
