## Изменения

- Исправлен сброс пользовательской паузы хоста при сохранении мира для подключения клиента. Завершение сохранения больше не считается нажатием игрока.
- Хост ждёт READY конкретной загрузки; если до подключения была включена пользовательская пауза, после READY она сохраняется.
- Настоящие нажатия паузы и кнопок скорости продолжают менять общее состояние сессии.
- Снимки сущностей остаются на **100 Гц** (цель отправки, 10 мс); максимальное ожидание ENet в простое — **1 мс**. Частота свежих состояний зависит от FPS; остальные каналы не переводятся на 100 Гц.

## Совместимость

Формат пакетов не изменён: **public — протокол 64, dev — протокол 65**. Версии v0.1.24–v0.1.26 совместимы внутри соответствующей ветки; для исправленной паузы обновите хост, рекомендуется обновить и клиентов. Public и dev между собой не соединяются.

## Установка и обновление

Скачайте `KenshiCoop-kit.zip`. Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов на хосте и клиентах. RE_Kenshi должен быть установлен. Свой `coop_config.json` сохраните.

В архиве только четыре файла:

- `KenshiCoop.dll`
- `KenshiCoopUI.dll`
- `KenshiCoop.mod`
- `RE_Kenshi.json`

Обе DLL заменяются вместе; изменения действуют после перезапуска игры.

<details><summary>English</summary>

Fixed host user-pause loss when saving the world for a connecting client. Save completion no longer counts as a player action. The host still waits for the READY matching the current load; an existing user pause remains active afterward. Real pause and speed-button actions still update the session.

Entity snapshots still target **100 Hz** (10 ms); ENet's maximum idle wait remains **1 ms**. Fresh captures depend on game FPS; other channel intervals are unchanged.

Packet formats are unchanged: public protocol **64**, private protocol **65**. v0.1.24–v0.1.26 interoperate within their respective build families. Update the host for the pause fix; updating clients is recommended. Public/private pairs do not connect.

With Kenshi closed, copy the `KenshiCoop/` folder from `KenshiCoop-kit.zip` into `<Kenshi>/mods/`, replacing both DLLs, the mod and RE_Kenshi JSON. RE_Kenshi must already be installed. Keep the existing `coop_config.json` and restart the game.

</details>
