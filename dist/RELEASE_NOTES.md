## Ник, перенос тел, дюп лута

Четыре правки по логам сессии 12:28–12:34. Три из них — один и тот же корень.

### Ник ставился как `nickedit`

Поле ввода отдавало **ключ строки** вместо набранного текста: при пустом чтении
срабатывала запасная ветка, которая брала служебный идентификатор `nickedit`
(поле UDP так же отдало бы `udpedit`). Ветка убрана — ключ не может быть
значением ни при каких условиях.

Заодно пустое чтение больше не затирает уже запомненный ник: «поле ничего не
сказало» и «игрок стёр ник» — разные вещи.

### Тела не переносились между машинами

Событие называет тело **рукой автора**. Для тела из сейва она разрешается у
обоих, но NPC, созданный по ходу игры, существует у соседа как прокси с **другой
локальной рукой**:

```
[rekey] wire=2,3137817344 local=1,1228515584
```

Обработчики резолвили руку с провода напрямую — и потому не делали ничего именно
для тех тел, ради которых нужны: перенесённых трупов, оглушённых рейдеров. В
логе это выглядело как `ok=0` и оседание, где все записи трансформа нули:

```
[carry] RECV PICKUP carried=1,656982400 ok=0
[carry] SETTLE recv park=0 raw=0 vis=0 hkset=0 restore=0 mv=0.0,0.0,0.0
```

Добавлен перевод руки в локальный экземпляр, применён к подъёму и сбросу тела,
посадке после броска, оживлению и потере конечности. У нокаута и смерти такой
путь уже был, но отдавал закэшированный указатель без перепроверки — теперь
идёт через движок, и прокси, чей блок выгрузился, читается как отсутствующий.

Изменение строго аддитивное: где резолв работал, ничего не меняется; меняется
только «не разрешилось» → «разрешилось через прокси».

### Медицина молча выбрасывалась для тех же тел

Канал витальных показателей резолвил руку так же напрямую и на неудаче **тихо
пропускал пакет** — без единой строки лога. Отсюда 436 отправок и полная
невозможность понять, дошло ли хоть что-то. Тот же перевод руки плюс
диагностика: `[med] APPLY` и `[med] APPLY-MISS`, не чаще раза в 5 секунд на
тело.

### Дюп лута при открытом окне у обоих

Отсрочка применения (фикс вылета) останавливала **применение**, но не
**публикацию**. Сторона с открытой панелью 35 секунд не удаляла забранное и
продолжала рассылать свой устаревший снимок; сосед его применял, и reconcile
**создавал** предметы обратно — остаток скакал `1 → 2 → 1 → 0 → 1`.

Теперь контейнер с неприменённым снимком не публикуется. Ничего не теряется:
сосед продолжает слать то, что держит сам, и после закрытия окна обе стороны
сходятся. Немой контейнер самоисцеляется — состояние сверяется с живой панелью,
а не только с флагом.

### Что это не чинит

- **Join публикует NPC, которых не авторит** (три события и один медпакет за
  сессию, включая нокаут для собственных прокси). Вероятная вторая половина
  «трупа, который встал».
- **Набор ника в поле** остаётся непроверенным: правка убирает `nickedit`, но
  если текст не доходит до поля, ник будет пустым. Проверяется строкой
  `[nick] applied id=… '<имя>'` в логе.
- Трое и четверо игроков.

### Что смотреть в логе

`[carry] RECV PICKUP … ok=1` вместо `ok=0`, и ненулевые `park/raw/vis` в
`SETTLE recv`. Плюс появление `[med] APPLY` — или `[med] APPLY-MISS`, если тела
всё ещё не разрешаются.

### Установка

При включённых обновлениях апдейтер подтянет сам. Иначе при закрытой игре
положите три файла в `<Kenshi>\mods\KenshiCoop\`: `KenshiCoop.dll`,
`RE_Kenshi.json`, `KenshiCoop.mod`. Нужны Kenshi 1.0.65 и
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

---

<details>
<summary><b>🇬🇧 Read this in English</b></summary>

## Nick, body carrying, loot duplication

Four fixes from the 12:28-12:34 session logs. Three share one root cause.

### The nick was applied as `nickedit`

The text field handed back the row's **key** instead of typed text: an empty
read fell through to a fallback that took the internal id `nickedit` (the UDP
field would likewise have produced `udpedit`). The fallback is gone - a key can
never be a value.

An empty read also no longer wipes a remembered nick: "the field said nothing"
and "the player cleared it" are different things.

### Bodies were not carried across machines

An event names a body by the **author's** hand. For a body from the save that
hand resolves on both machines, but an NPC spawned at runtime exists on the peer
as a proxy under a **different local hand**:

```
[rekey] wire=2,3137817344 local=1,1228515584
```

Handlers resolved the wire hand directly, so they did nothing for exactly the
bodies that need it - carried corpses, knocked-out raiders. In the log that read
as `ok=0` and a settle whose transform writes were all zero:

```
[carry] RECV PICKUP carried=1,656982400 ok=0
[carry] SETTLE recv park=0 raw=0 vis=0 hkset=0 restore=0 mv=0.0,0.0,0.0
```

A remap to the local instance was added and applied to body pickup and drop, the
post-throw landing settle, revive, and limb loss. Knockout and death already had
such a path but handed back a cached pointer without revalidating - it now goes
back through the engine, so a proxy whose block unloaded reads as absent.

Strictly additive: where the resolve worked nothing changes; only "failed to
resolve" becomes "resolved through the proxy".

### Medical was silently dropped for the same bodies

The vitals channel resolved the hand the same direct way and, on failure,
**dropped the packet without a single log line**. Hence 436 sends and no way to
tell whether anything arrived. Same remap, plus diagnostics: `[med] APPLY` and
`[med] APPLY-MISS`, at most once per body per 5 s.

### Loot duplicated when both players had the window open

The apply deferral (the crash fix) stopped **applying** but not **publishing**.
The side with a panel open went 35 s without removing what the other had taken
while still broadcasting its stale snapshot; the peer applied it and the
reconcile **created** the items back - the remaining count oscillated
`1 -> 2 -> 1 -> 0 -> 1`.

A container with an unapplied snapshot is no longer published. Nothing is lost:
the peer keeps publishing what it holds, and both sides converge once the window
closes. The mute self-heals - it is confirmed against the live panel, not just a
flag.

### What this does not fix

- **The join publishes NPCs it does not author** (three events and one medical
  packet in that session, including a knockout for its own proxy). Likely the
  other half of "the corpse stood up".
- **Typing into the nick field** remains unverified: this removes `nickedit`,
  but if text never reaches the field the nick will be empty. Check for
  `[nick] applied id=… '<name>'` in the log.
- Three and four players.

### What to look for in the log

`[carry] RECV PICKUP … ok=1` instead of `ok=0`, and non-zero `park/raw/vis` in
`SETTLE recv`. Plus `[med] APPLY` appearing - or `[med] APPLY-MISS` if bodies
still do not resolve.

### Install

With updates enabled the updater fetches this. Otherwise, with the game closed,
put three files in `<Kenshi>\mods\KenshiCoop\`: `KenshiCoop.dll`,
`RE_Kenshi.json`, `KenshiCoop.mod`. Requires Kenshi 1.0.65 and
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847).

</details>
