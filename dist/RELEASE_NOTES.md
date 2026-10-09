## Изменения

- Целевая частота отправки снимков сущностей повышена до **100 Гц** в public и dev: интервал уменьшен до **10 мс**.
- Расписание отправки использует монотонный QPC-таймер вместо грубого `GetTickCount`. Снимки сохраняют время захвата; буфер отправки переиспользуется между отправками.
- Ожидание ENet в простое остаётся **1 мс**. Пауза хоста до READY конкретной загрузки и предыдущие исправления подключения сохранены.
- Ротация дальних NPC остаётся на 50 мс; отдельные интервалы инвентаря, сейвов и часов не изменены. Это повышение частоты снимков, а не всех сетевых каналов.

Свежие состояния захватываются игровым потоком, поэтому их частота зависит от FPS. 100 Гц — цель планировщика отправки, не гарантия пинга или 100 новых состояний мира в секунду. Более частая отправка увеличивает сетевую нагрузку.

## Совместимость

Формат пакетов не изменён: **public — протокол 64, dev — протокол 65**. Внутри соответствующей ветки v0.1.24 и v0.1.25 совместимы; для отправки на 100 Гц в обе стороны обновите хоста и клиентов. Public и dev между собой не соединяются.

## Установка и обновление

Скачайте `KenshiCoop-kit.zip`. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов на хосте и клиентах. RE_Kenshi должен быть установлен. Свой `coop_config.json` сохраните.

В архиве только четыре файла:

- `KenshiCoop.dll`
- `KenshiCoopUI.dll`
- `KenshiCoop.mod`
- `RE_Kenshi.json`

Обе DLL заменяются вместе; изменения действуют после перезапуска игры.

<details><summary>English</summary>

The entity-snapshot sender now targets **100 Hz** in public and dev, with a **10 ms** interval paced by the monotonic QPC clock instead of coarse `GetTickCount`. Capture-time stamps are preserved and the send buffer is reused between sends. ENet's maximum idle wait remains 1 ms; the load-specific READY host pause and prior connection fixes remain.

The distant-NPC slice still rotates every 50 ms. Inventory, save and clock channel intervals are unchanged. Fresh captures remain game-frame-paced: the 100 Hz sender target is not a ping guarantee or a promise of 100 new world states per second. More frequent sends increase network load.

Packet formats are unchanged: public protocol **64**, private protocol **65**. v0.1.24 and v0.1.25 interoperate within their respective build families; update both host and clients for bidirectional 100 Hz sending. Public/private pairs do not connect.

With Kenshi closed, copy the `KenshiCoop/` folder from `KenshiCoop-kit.zip` into `<Kenshi>/mods/`, replacing both DLLs, the mod and RE_Kenshi JSON. RE_Kenshi must already be installed. Keep the existing `coop_config.json` and restart the game.

</details>
