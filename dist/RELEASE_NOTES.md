## Торговцы больше не остаются с пустым прилавком

У торговца нет товара. Причина в логе, и она измерима.

Kenshi наполняет прилавок лавки и бункер станка **лениво** — пока рядом никто не
стоял, содержимого просто нет. Хост объявляет соседу содержимое всех складов и
станков вокруг себя, и если его движок ещё не наполнил прилавок, уходит снимок
«пусто». Сосед честно применяет: **уничтожает у себя весь товар**. А дальше
страховочная пересылка повторяет тот же пустой снимок каждые пять секунд, поэтому
прилавок остаётся голым — восстановиться ему не дают.

Масштаб за одну вашу сессию:

```
хост:    83 контейнера из 106 объявили items=0 первым же снимком
клиент:  440 применений пустого снимка
```

Пустота — это факт про **машину**, а не про мир. Теперь контейнер, который мы
объявляем своим только потому, что стоим рядом (лавка, станок, труп), не имеет
права утверждать пустоту, пока мы **хоть раз не видели в нём содержимое**.
Собственных карманов отряда это не касается: там пусто — значит игрок вынес, и
это надо передать.

В лог добавлена строка `[inv] CENSUS-MUTE`.

### 0.1.17 проверен, но не полностью

Новая проверка сработала:

```
[wd] decrease-moved ... sid='52295-rebirth.mod' (still in a container, never on the ground; no drop authored)
```

Фантомных выбросов было 6 у хоста и 4 у клиента, стало 0 и 1. Но **один всё-таки
прошёл** — и хост под него сфабриковал вещь (`APPLY-HEALED`). Проверка подавляет
выброс только по положительному чтению предмета; когда указатель прочитать не
удалось, работает старый путь. Значит дюп стал редким, а не исчез.

### NPC у хоста и у клиента: измерил, править не стал

Расхождение реальное и причина у него не в NPC, а в том, **кто чем владеет**:

```
хост:    enum=69  notmine=68  proxyrow=36   mine=1   hid=22  supp=22
клиент:  wide=47              drv=17        mine=29  hid=0   supp=0
```

У хоста 69 тел, и он считает своими **одно**. 22 своих NPC он прячет, потому что
их область объявил своей клиент, и вместо них показывает 36 копий, присланных
клиентом. У клиента при этом своих — 29.

Это работает механизм «владения по клеткам»: клетка достаётся тому, кто в ней
стоял, и остаётся за ним после ухода. Клиент зашёл в город первым — город его.
Формально всё отработало как задумано, фактически хост перестал быть хозяином
мира, и населённость у вас разъехалась.

Гадать, как это перекроить, я не буду — это решение про устройство мода, а не
правка. Но есть бесплатная проверка: **выключите владение по клеткам на обеих
машинах** и сыграйте сессию.

```bash
setx KENSHICOOP_CELL_AUTH 0
```

Тогда мир целиком остаётся за хостом. Если разница NPC уходит — значит виноват
именно этот механизм, и дальше решаем: чинить его или оставить выключенным. Если
не уходит — причина другая, и я её искал не там. Ставить надо **обоим**, и это
переменная окружения, поэтому Kenshi после неё надо перезапустить.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

<details>
<summary>🇬🇧 English</summary>

## Traders no longer end up with an empty counter

A trader has no goods. The log explains it, and the effect is measurable.

Kenshi stocks a shop's shelf and a machine's output bin **lazily** — until someone
has stood nearby, there simply is no content. The host announces the contents of
every store and machine around it, and if its engine has not stocked the shelf
yet, an "empty" snapshot goes out. The peer faithfully applies it and **destroys
its own stock**. The safety resend then repeats that empty snapshot every five
seconds, so the counter stays bare — it is never allowed to recover.

Over one of your sessions:

```
host:  83 of 106 authored containers announced items=0 on their first send
join:  440 empty snapshots applied
```

Emptiness is a fact about the **machine**, not about the world. A container we
author only because we happen to stand near it (a shop, a machine, a corpse) may
no longer assert emptiness until we have seen it hold something **at least once**.
Squad pockets are unaffected: empty there means the player emptied it, and that
must cross.

New log line: `[inv] CENSUS-MUTE`.

### 0.1.17 verified, but not completely

The new check does fire:

```
[wd] decrease-moved ... sid='52295-rebirth.mod' (still in a container, never on the ground; no drop authored)
```

Phantom drops went from 6 (host) and 4 (join) to 0 and 1. But **one still got
through**, and the host fabricated an item for it (`APPLY-HEALED`). The check
suppresses only on a positive read of the object; when the pointer cannot be read,
the old path still runs. So the duplicate is now rare, not gone.

### NPCs on host vs client: measured, not changed

The divergence is real, and its cause is not the NPCs but **who owns what**:

```
host:  enum=69  notmine=68  proxyrow=36   mine=1   hid=22  supp=22
join:  wide=47              drv=17        mine=29  hid=0   supp=0
```

The host has 69 bodies and considers **one** of them its own. It hides 22 of its
own NPCs because the client claimed their region, and shows 36 client-streamed
copies instead. The client, meanwhile, owns 29.

This is per-cell authority doing its job: a cell goes to whoever stood in it and
stays with them after they leave. The client entered the town first, so the town
is theirs. Formally correct; in practice the host stopped being the world's owner
and your populations drifted apart.

I am not going to guess at a redesign — that is a decision about how the mod
works, not a fix. But there is a free experiment: **turn per-cell authority off on
both machines** and play a session.

```bash
setx KENSHICOOP_CELL_AUTH 0
```

The whole world then stays with the host. If the NPC difference goes away, this
mechanism is the cause and we decide whether to fix it or leave it off. If it does
not, the cause is elsewhere and I was looking in the wrong place. Both machines
need it, and it is an environment variable, so restart Kenshi afterwards.

### Install

With updates on, the updater fetches it. Otherwise, with the game closed, drop the
three files into `<Kenshi>\mods\KenshiCoop\`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
