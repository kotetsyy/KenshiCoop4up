## Изменения

- Исправлено дублирование уже лежащих в мире предметов при их загрузке у другого игрока.
- Исправлен повторный лут NPC, сундуков и рабочих станций. При одновременном взятии последнего предмета подтверждается только реально доступное количество, лишняя клиентская копия откатывается.
- Исправлена синхронизация добытой меди и разделения стопок руды.
- Содержимое сундуков, включая положенную еду, синхронизируется с другими игроками.
- Рюкзак передаётся вместе с содержимым — при отдаче и взятии у другого игрока.
- Исправлены пустые магазины у клиента и различающийся ассортимент, включая походный магазин Скуина и оба магазина соседней путевой станции. Торговец сохраняет шаблон отряда хоста, складские стопки — его расположение; учитываются характеристики вещей и разные чертежи.
- Сохранены пауза хоста до READY текущей загрузки, целевая частота снимков сущностей **100 Гц** и максимальное ожидание ENet в простое **1 мс**.

## Совместимость

**Публичный протокол — 70. Хост и все клиенты должны обновиться до v0.1.27.** С предыдущими публичными сборками и dev-протоколами 67/69/71 соединения нет.

Этот релиз исправляет критические ошибки предметов. Рассинхрон HP, голода, замков, поз и движения NPC не заявляется исправленным.

## Установка и обновление

Скачайте **`KenshiCoop-kit.zip`**. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов на хосте и клиентах. RE_Kenshi должен быть установлен. Существующий `coop_config.json` сохраните.

В архиве только `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoop.mod` и `RE_Kenshi.json`. Обе DLL заменяются вместе; изменения действуют после перезапуска игры.

<details><summary>English</summary>

Fixed duplicate streamed ground items, repeated NPC/container/workstation loot, competing takes of the last item, mined-ore synchronization and split stacks. Chest deposits now converge; backpack transfers preserve their contents in both directions. Merchants retain their actual host platoon template; stock uses the actual shop furniture, native stack positions and item provenance, including distinct blueprints. This fixes Squin's travel shop and both nearby Waystation shops.

The host's load-specific READY pause, **100 Hz** entity snapshot target and **1 ms** maximum idle ENet wait remain unchanged.

**Public protocol 70: update the host and every client to v0.1.27.** Older public builds and private protocols 67/69/71 are incompatible. HP, hunger, locks, poses and NPC movement desyncs are not claimed fixed in this release.

With Kenshi closed, copy `KenshiCoop/` from **`KenshiCoop-kit.zip`** into `<Kenshi>/mods/`, replacing the four mod files. RE_Kenshi must already be installed. Keep the existing `coop_config.json` and restart the game.

</details>
