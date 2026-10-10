## Изменения

- В нижнюю строку панели F2 добавлена кнопка **Discord**: открывает канал сообщества в браузере.
- На узких окнах версия и подсказка вынесены в отдельную строку, чтобы кнопки не перекрывались.
- Техническое описание мода в лаунчере заменено коротким: совместная игра до четырёх игроков, управление своим отрядом, вход через F2 и предупреждение о возможных рассинхронах. Название и папка `KenshiCoop` сохранены.

## Совместимость

Сетевая совместимость с **v0.1.27** сохранена. Исправления предметов из v0.1.27, пауза хоста до READY текущей загрузки, целевая частота снимков **100 Гц** и ожидание ENet в простое до **1 мс** не изменены. Новых исправлений рассинхрона HP, голода, замков и NPC в этом обновлении нет.

## Установка и обновление

Скачайте **`KenshiCoop-kit.zip`**. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов. RE_Kenshi должен быть установлен. Существующий `coop_config.json` сохраните.

В архиве только `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoop.mod` и `RE_Kenshi.json`. Автообновление заменяет обе DLL, включая UI; для нового описания в лаунчере замените также `KenshiCoop.mod` из полного комплекта. После обновления перезапустите игру.

<details><summary>English</summary>

Added a **Discord** button to the F2 footer. Narrow windows place the version and hint on a separate row. Replaced the launcher's technical mod description with a short introduction; the `KenshiCoop` name and folder are unchanged.

Compatible with **v0.1.27**. Its item fixes, load-specific READY pause, **100 Hz** entity snapshot target and **1 ms** maximum idle ENet wait are unchanged. This update does not add fixes for HP, hunger, locks or NPC desyncs.

With Kenshi closed, copy `KenshiCoop/` from **`KenshiCoop-kit.zip`** into `<Kenshi>/mods/`, replacing the four mod files. RE_Kenshi must already be installed. Keep your `coop_config.json`. Auto-update replaces both DLLs, including the UI; install the `.mod` from the full kit for the new launcher description. Restart the game afterward.

</details>
