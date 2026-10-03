## Новое нативное окно совместной игры

Раньше ввод и вставка были ограничены, UDP-порт терялся при смене роли, а часть диагностики не помещалась в окно. В F2 теперь полноценные редактируемые поля и две колонки в оформлении Kenshi.

- Ник и адрес: кириллица, выделение, редактирование, Ctrl+V и Tab; игровые клавиши отключены, пока поле в фокусе.
- Хост/клиент и Steam/прямой IP — отдельные равноправные переключатели. Основное действие выделено; неверные значения объясняются рядом с полем.
- Справа — реальные этапы подключения, прогресс передачи мира и игроки. Диагностика свёрнута, полный отчёт копируется отдельно. Ошибки обновления не подменяют состояние соединения.
- UDP-порт сохраняется при смене роли. Слишком длинная вставка отклоняется, а не обрезается молча. F2/Esc работают только в активной копии игры и не отключают сеть.
- Интерфейс вынесен в `KenshiCoopUI.dll`; сеть и состояние мира остаются в `KenshiCoop.dll`. Убрана повторная регистрация панели у владельца GUI, приводившая к двойному удалению.

**Установка:** скачайте `KenshiCoop-kit.zip` и перенесите папку `KenshiCoop` в `mods` при закрытой игре. При ручном обновлении заменяйте **обе DLL из одной сборки**. Со старой сборки, в которой была только одна DLL, предпочтительна ручная установка пары: старый автообновлятор загружает только основной плагин; новый плагин доставит отсутствующую UI при следующей проверке, после чего потребуется ещё один перезапуск.

**Проверка:** публичная Release-пара собрана с v100; `prototest` — 566/566. Новый F2 открыт в настоящей игре с версией v0.1.21. Общий интерфейс ранее проверен при 1280×720 и 1920×1080; приватная диагностическая пара передавала мир и повторно доходила до READY (`badCrc=0`). Эта проверка не означает проверенного Steam P2P между двумя аккаунтами. Публичная и приватная диагностическая сборки не совместимы между собой.

**Осталось:** выход через WM_CLOSE давал `0xC0000409` со стеком `RE_Kenshi.dll+0x381e9`, в том числе без UI DLL. RE_Kenshi не изменён; этот сбой не объявляется исправленным. Горячей замены DLL нет; прирост FPS не обещается.

Соответствующие исходники этой сборки по AGPL-3.0 находятся в `KenshiCoop-source.zip` этого релиза.

<details><summary>English</summary>

The native two-column F2 panel now supports editable Cyrillic nicknames and addresses, selection, editing, Ctrl+V and Tab, with game hotkeys suppressed during input. It shows real connection/world-transfer stages, players, collapsed diagnostics, a complete copy-report and a separate updater section. The UDP port survives role changes; oversized paste is rejected. F2/Esc affect only the foreground game and never disconnect.

Install the KenshiCoop folder from KenshiCoop-kit.zip into mods while the game is closed. Always replace both KenshiCoop.dll and KenshiCoopUI.dll together. For a first upgrade from a core-only release, manual installation of both is recommended: the old updater downloads only the core; the new core repairs the missing UI on its next check and requires another restart.

The public v100 Release pair builds successfully; prototest passes 566/566. The v0.1.21 F2 panel was opened in the real game. Earlier shared-UI verification covered both resolutions and real/repeated world transfer with the private diagnostic pair. Two-account Steam P2P was not exercised; public and private diagnostic builds cannot connect to each other.

Known limitation: WM_CLOSE produced 0xC0000409 with RE_Kenshi.dll+0x381e9 in the stack, including without a UI DLL. RE_Kenshi is unchanged; this failure is not claimed fixed. No hot reload or FPS gain is promised. Corresponding AGPL-3.0 sources are provided in KenshiCoop-source.zip.

</details>
