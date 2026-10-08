## Изменения

- В public перенесён барьер загрузки из dev: хост держит свою симуляцию на паузе до появления загруженного мира у всех подключённых клиентов. Окончание передачи файлов само по себе больше не снимает эту паузу.
- READY содержит идентификатор конкретного `LOAD_GO`. Подтверждение предыдущей загрузки не разрешает текущую; READY отправляется только после появления живого мира, включая подключение из главного меню и перезагрузку уже открытого мира.
- Временная пауза не заменяет выбранную скорость или пользовательскую паузу. Барьер работает при включённых save/load/speed/time sync; при отключении клиента его ожидание убирается.
- Максимальное ожидание ENet в простое уменьшено до **1 мс** в public и dev. Снимки сущностей по-прежнему отправляются на 20 Гц. Это не гарантия пинга 1 мс или измеренного ускорения; более частые пробуждения могут увеличить нагрузку CPU.
- Сохранены исправления Unicode-путей сейвов и ошибок bootstrap, атомарный приём блоков передачи мира и поле ника без отдельной кнопки вставки.
- Пересылка клиентского лога хосту остаётся только в dev и не включена в public.

## Совместимость

Public использует **протокол 64**: в `TimePacket` теперь всегда есть `readyLoadId`.
Dev этой версии использует **протокол 65** с дополнительным зеркалом клиентского лога.
Старые public 59 и dev 63 отвергаются при рукопожатии. Public и dev друг с другом не соединяются. Установите одну и ту же пару DLL на хоста и всех клиентов.

## Установка и обновление

Скачайте `KenshiCoop-kit.zip`. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов **на обеих машинах**.

В архиве только четыре runtime-файла:

- `KenshiCoop.dll`
- `KenshiCoopUI.dll`
- `KenshiCoop.mod`
- `RE_Kenshi.json`

RE_Kenshi должен быть установлен. Свой `coop_config.json` сохраните; новый конфиг создаётся через F2. Лаунчеров, `PROVENANCE.json`, инструкций и исходников внутри игрового архива нет. Обе DLL обновляются вместе, изменения действуют после перезапуска игры.

<details><summary>English</summary>

The public build now shares the development build's join-load barrier: the host's simulation is held until every connected client has a live world and reports READY for its current `LOAD_GO` id. File-transfer completion or a previous load's READY does not release the barrier. Title-screen joins and in-world reloads are covered by the same implementation. The temporary hold preserves the speed/pause vote and requires save, load, speed and time sync; disconnected clients are removed from the wait.

Both configurations use a 1 ms maximum idle ENet wait. Entity snapshots remain at 20 Hz. This is not a 1 ms ping guarantee or a measured performance improvement; CPU wakeups may increase. Unicode save-path, bootstrap failure and atomic world-transfer fixes remain, as does the full-width nickname input without its separate Paste button. Client-log mirroring remains private-only.

Public protocol is **64** (`TimePacket` always contains `readyLoadId`); private protocol is **65** with log mirroring. Earlier public 59/private 63 builds and mixed public/private pairs are rejected. Install the same DLL pair on the host and all clients with Kenshi closed.

`KenshiCoop-kit.zip` contains only the core DLL, UI DLL, mod and RE_Kenshi JSON under `KenshiCoop/`. Copy the folder into `<Kenshi>/mods/`, keep the existing `coop_config.json` and restart. RE_Kenshi must already be installed. No launchers, provenance, notes or source archives are bundled.

</details>
