## Поле ника без лишней кнопки

- Убрана кнопка «Вставить» рядом с ником. Поле ника теперь занимает всю ширину строки.
- Обычный ввод, кириллица, выделение, редактирование, Ctrl+V и переход по Tab сохранены.
- Кнопки вставки Steam ID, адреса и порта не изменены.
- Исправления подключения и передачи мира из v0.1.22 сохранены.

**Установка:** при закрытой игре на обеих машинах скопируйте папку `KenshiCoop` из `KenshiCoop-kit.zip` в `<Kenshi>\mods\` с заменой файлов. В архиве только четыре файла: `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoop.mod`, `RE_Kenshi.json`. Свой `coop_config.json` сохраняйте. RE_Kenshi должен быть установлен; включите KenshiCoop в меню Mods.

**О номере протокола:** версия сборки — v0.1.23. `proto` обозначает формат сетевых пакетов, а не возраст сборки. Public сохраняет протокол 59; dev использует 63 с дополнительным приватным форматом диагностики и пересылкой логов клиента хосту. Они несовместимы между собой. Удаление кнопки не меняет сетевой формат.

**Проверка:** пары Release и Harness собраны с v100. По просьбе пользователя игровые тесты не запускались; внешний вид этой правки в запущенной игре не объявляется проверенным.

Исходники по AGPL-3.0 доступны в [теге v0.1.23](https://github.com/kotetsyy/KenshiCoop4up/tree/v0.1.23); [инструкции сборки](https://github.com/kotetsyy/KenshiCoop4up/blob/main/docs/BUILD_SETUP.md). Отдельный source-архив к релизу не прикладывается.

<details><summary>English</summary>

Removed the Paste button beside the nickname and expanded the input to the full row width. Native typing, Cyrillic, selection/editing, Ctrl+V and Tab navigation are retained. Endpoint Paste buttons are unchanged, as are the v0.1.22 connection/save-transfer fixes.

Close Kenshi on both machines and install the KenshiCoop folder from KenshiCoop-kit.zip into mods/. The archive contains only the core DLL, UI DLL, mod and RE_Kenshi JSON. Replace the pair together and preserve coop_config.json. RE_Kenshi is required.

Build version is v0.1.23. Protocol numbers identify the network format, not build age: public remains 59, while private dev is 63 with additional diagnostics/client-log streaming. They cannot connect to each other. This UI change does not alter the wire format.

Both Release and Harness pairs build with v100. In-game tests were skipped at the user's request; this layout change is not claimed visually verified. Sources are available at the [v0.1.23 repository tag](https://github.com/kotetsyy/KenshiCoop4up/tree/v0.1.23), with [build instructions](https://github.com/kotetsyy/KenshiCoop4up/blob/main/docs/BUILD_SETUP.md). No separate source archive is attached.

</details>
