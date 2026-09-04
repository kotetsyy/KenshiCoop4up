## Лог перестал раздуваться

Тестовая телеметрия попадала в релизную сборку. В сессии 12:28–12:34 join-лог
весил **3.8 МБ против 561 КБ** у хоста, и 30 тысяч строк из 34 тысяч были не о
игре:

| строк | что это |
|---|---|
| 20 965 | `SCENARIO PROXY` — вход для оракула `spawn_sync`, 2 Гц на каждый прокси |
| 9 053 | `[proxy] drift` — замер расхождения, 1 Гц на каждый прокси |

Обе строки нужны автотестам и бесполезны игроку. Теперь они собираются только в
Harness-сборке (`KENSHICOOP_HARNESS`), которой релиз не является. Оракулы
`Motion` / `World` / `Combat` продолжают их получать — проверено сборкой обеих
конфигураций.

Полезные строки не тронуты: `[census]`, `[spawn]`, `[life]`, `[inv]`,
`[carry]`, `[med]`, `[interp]`, `[audit]` остаются. Лог должен стать примерно
**в семь раз меньше** и наконец быть пригодным для отправки и чтения.

Изменений в игровой логике нет.

---

<details>
<summary><b>🇬🇧 Read this in English</b></summary>

## The log stopped bloating

Test telemetry was shipping in the release build. In the 12:28-12:34 session the
join log was **3.8 MB against the host's 561 KB**, and 30k of its 34k lines were
not about the game:

| lines | what |
|---|---|
| 20,965 | `SCENARIO PROXY` - input for the `spawn_sync` oracle, 2 Hz per proxy |
| 9,053 | `[proxy] drift` - divergence measurement, 1 Hz per proxy |

Both feed the automated tests and are useless to a player. They now compile only
into the Harness build (`KENSHICOOP_HARNESS`), which a release is not. The
Motion / World / Combat oracles still receive them - verified by building both
configurations.

Useful lines are untouched: `[census]`, `[spawn]`, `[life]`, `[inv]`, `[carry]`,
`[med]`, `[interp]`, `[audit]` all remain. Logs should be about **seven times
smaller** and finally practical to send and read.

No gameplay changes.

</details>
