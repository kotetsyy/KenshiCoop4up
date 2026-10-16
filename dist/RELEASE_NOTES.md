## Изменения

- Исправлено перетаскивание товаров при покупке и продаже у торговца: передаётся выбранное количество, а итог сделки определяет **HOST**. Защита штатного списания/зачисления денег и предмета на курсоре предотвращает повторную оплату и потерю остатка стопки при подтверждении сделки.
- Steam и прямой IP переведены на **GameNetworkingSockets (GNS)** вместо ENet. Транспорт выбирается явно; ошибка Steam не включает UDP автоматически. Ручной список разрешённых SteamID больше не нужен.
- Автообновление больше не пересекается с загрузкой UI и сетевых DLL. После установки обновления новое подключение требует перезапуска Kenshi.

## Совместимость и ограничения

Публичная сборка использует **протокол 80** и несовместима с **0.1.x**. Всем игрокам нужна одна версия. Приватный Harness использует **протокол 81**, не соединяется с публичной сборкой и не получает автообновления.

Это не исправление всех рассинхронов: остаются проблемы окружения, экипировки, NPC, времени/координат и других игровых состояний. Steam P2P между двумя реальными аккаунтами и реальные сессии на 3–4 игроков ещё не сертифицированы.

## Установка и обновление

Скачайте **`KenshiCoop-kit.zip`** из [последнего релиза](https://github.com/kotetsyy/KenshiCoop4up/releases/latest). Полностью закройте Kenshi и скопируйте папку `KenshiCoop` в `<Kenshi>\mods\` с заменой файлов. RE_Kenshi должен быть установлен. Сохраните свой `coop_config.json` и сейвы.

**Первый переход с 0.1.x обязателен вручную, полным комплектом:** старый updater не поддерживает новую схему. В комплекте ровно шесть файлов: `KenshiCoop.dll`, `KenshiCoopUI.dll`, `KenshiCoopNetSteam.dll`, `KenshiCoopNetUDP.dll`, `KenshiCoop.mod` и `RE_Kenshi.json`.

После перехода новый updater заменяет **все четыре DLL одной сборки**, включая UI, для следующего запуска. `KenshiCoop.mod` и `RE_Kenshi.json` устанавливаются из полного комплекта.

<details><summary>English</summary>

Merchant drag-and-drop purchases and sales now use the selected quantity and the **HOST**'s authoritative result, with native payment and cursor-stack protection against duplicate payment and loss of the remaining stack.

Steam and direct IP now use **GameNetworkingSockets (GNS)** instead of ENet, without automatic transport fallback or a manual SteamID allow-list. Public protocol **80** is incompatible with **0.1.x** and private Harness protocol **81**; Harness updates are disabled. All players must use the same version.

The first migration from 0.1.x **must be manual**: close Kenshi and install all six files from **`KenshiCoop-kit.zip`** in the latest release into `<Kenshi>/mods/KenshiCoop/`. RE_Kenshi is required. Preserve config and saves. The new updater subsequently replaces all four DLLs together, including UI; install `.mod` and `RE_Kenshi.json` from the full kit.

Environment, outfit, NPC, time/position and other desync issues remain. Actual two-account Steam P2P and real 3–4-player sessions are not yet certified.

</details>
